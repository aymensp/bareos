/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2015-2017 Planets Communications B.V.

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/
/*
 * Chunked volume device abstraction.
 *
 * Marco van Wieringen, February 2015
 */

#include "include/bareos.h"
#include "lib/edit.h"

#if defined(HAVE_DROPLET)
#include "stored.h"
#include "chunked_device.h"

#ifdef HAVE_MMAP
#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif
#endif

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * This implements a device abstraction that provides so called chunked
 * volumes. These chunks are kept in memory and flushed to the backing
 * store when requested. This class fully abstracts the chunked volumes
 * for the upper level device. The stacking for this device type is:
 *
 * <actual_device_type>::
 *          |
 *          v
 *   chunked_device::
 *          |
 *          v
 *       Device::
 *
 * The public interfaces exported from this device are:
 *
 * set_inflight_chunk() - Set the inflight flag for a chunk.
 * clear_inflight_chunk() - Clear the inflight flag for a chunk.
 * is_inflight_chunk() - Is a chunk current inflight to the backing store.
 * nr_inflight_chunks() - Number of chunks inflight to the backing store.
 * setup_chunk() - Setup a chunked volume for reading or writing.
 * read_chunked() - Read a chunked volume.
 * write_chunked() - Write a chunked volume.
 * close_chunk() - Close a chunked volume.
 * truncate_chunked_volume() - Truncate a chunked volume.
 * chunked_volume_size() - Get the current size of a volume.
 * load_chunk() - Make sure we have the right chunk in memory.
 *
 * It also demands that the inheriting class implements the
 * following methods:
 *
 * flush_remote_chunk() - Flush a chunk to the remote backing store.
 * read_remote_chunk() - Read a chunk from the remote backing store.
 * chunked_remote_volume_size - Return the current size of a volume.
 * truncate_remote_chunked_volume() - Truncate a chunked volume on the
 *                                    remote backing store.
 */

/*
 * Actual thread runner that processes IO request from circular buffer.
 */
static void *io_thread(void *data)
{
   char ed1[50];
   chunked_device *dev = (chunked_device *)data;

   /*
    * Dequeue from the circular buffer until we are done.
    */
   while (1) {
      if (!dev->dequeue_chunk()) {
         break;
      }
   }

   Dmsg1(100, "Stopping IO-thread threadid=%s\n",
         edit_pthread(pthread_self(), ed1, sizeof(ed1)));

   return NULL;
}

/*
 * Allocate a new chunk buffer.
 */
char *chunked_device::allocate_chunkbuffer()
{
   char *buffer = NULL;

#ifdef HAVE_MMAP
   if (use_mmap_) {
      buffer = (char *)::mmap(NULL, current_chunk_->chunk_size,
                              (PROT_READ | PROT_WRITE),
                              (MAP_SHARED | MAP_ANONYMOUS),
                              -1, 0);
      Dmsg1(100, "Mapped %ld bytes for chunk buffer\n", current_chunk_->chunk_size);
   } else {
#endif
      buffer = (char *)malloc(current_chunk_->chunk_size);
#ifdef HAVE_MMAP
   }
#endif

   Dmsg2(100, "New allocated buffer of %d bytes at %p\n", current_chunk_->chunk_size, buffer);

   return buffer;
}

/*
 * Free a chunk buffer.
 */
void chunked_device::free_chunkbuffer(char *buffer)
{
   Dmsg2(100, "Freeing buffer of %d bytes at %p\n", current_chunk_->chunk_size, buffer);

#ifdef HAVE_MMAP
   if (use_mmap_) {
      ::munmap(buffer, current_chunk_->chunk_size);
      Dmsg1(100, "Unmapped %ld bytes used as chunk buffer\n", current_chunk_->chunk_size);
   } else {
#endif
      free(buffer);

      /*
       * As we released a big memory chunk let the garbage collector run.
       */
      garbage_collect_memory();
#ifdef HAVE_MMAP
   }
#endif
}

/*
 * Free a chunk_io_request.
 */
void chunked_device::free_chunk_io_request(chunk_io_request *request)
{
   Dmsg2(100, "Freeing chunk io request of %d bytes at %p\n", sizeof(chunk_io_request), request);

   if (request->release) {
      free_chunkbuffer(request->buffer);
   }
   free((void *)request->volname);
   free(request);
}

/*
 * Start the io-threads that are used for uploading.
 */
bool chunked_device::start_io_threads()
{
   char ed1[50];
   uint8_t thread_nr;
   pthread_t thread_id;
   thread_handle *handle;

   /*
    * Create a new ordered circular buffer for exchanging chunks between
    * the producer (the storage driver) and multiple consumers (io-threads).
    */
   if (io_slots_) {
      cb_ = New(ordered_circbuf(io_threads_ * io_slots_));
   } else {
      cb_ = New(ordered_circbuf(io_threads_ * OQSIZE));
   }

   /*
    * Start all IO threads and keep track of their thread ids in thread_ids_.
    */
   if (!thread_ids_) {
      thread_ids_ = New(alist(10, owned_by_alist));
   }

   for (thread_nr = 1; thread_nr <= io_threads_; thread_nr++) {
      if (pthread_create(&thread_id, NULL, io_thread, (void *)this)) {
         return false;
      }

      handle = (thread_handle *)malloc(sizeof(thread_handle));
      memset(handle, 0, sizeof(thread_handle));
      handle->type = WAIT_JOIN_THREAD;
      memcpy(&handle->thread_id, &thread_id, sizeof(pthread_t));
      thread_ids_->append(handle);

      Dmsg1(100, "Started new IO-thread threadid=%s\n",
            edit_pthread(thread_id, ed1, sizeof(ed1)));
   }

   io_threads_started_ = true;

   return true;
}

/*
 * Stop the io-threads that are used for uploading.
 */
void chunked_device::stop_threads()
{
   char ed1[50];
   thread_handle *handle;

   /*
    * Tell all IO threads that we flush the circular buffer.
    * As such they will get a NULL chunk_io_request back and exit.
    */
   cb_->flush();

   /*
    * Wait for all threads to exit.
    */
   if (thread_ids_) {
      foreach_alist(handle, thread_ids_) {
         switch (handle->type) {
         case WAIT_CANCEL_THREAD:
            Dmsg1(100, "Canceling thread with threadid=%s\n",
                  edit_pthread(handle->thread_id, ed1, sizeof(ed1)));
            pthread_cancel(handle->thread_id);
            break;
         case WAIT_JOIN_THREAD:
            Dmsg1(100, "Waiting to join with threadid=%s\n",
                  edit_pthread(handle->thread_id, ed1, sizeof(ed1)));
            pthread_join(handle->thread_id, NULL);
            break;
         default:
            break;
         }
      }

      thread_ids_->destroy();
      delete thread_ids_;
      thread_ids_ = NULL;
   }
}

/*
 * Set the inflight flag for a chunk.
 */
bool chunked_device::set_inflight_chunk(chunk_io_request *request)
{
   int fd;
   PoolMem inflight_file(PM_FNAME);

   Mmsg(inflight_file, "%s/%s@%04d", me->working_directory, request->volname, request->chunk);
   pm_strcat(inflight_file, "%inflight");

   Dmsg3(100, "Creating inflight file %s for volume %s, chunk %d\n",
         inflight_file.c_str(), request->volname, request->chunk);

   fd = ::open(inflight_file.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0640);
   if (fd >= 0) {
      P(mutex);
      inflight_chunks_++;
      V(mutex);
      ::close(fd);
   } else {
      return false;
   }

   return true;
}

/*
 * Clear the inflight flag for a chunk.
 */
void chunked_device::clear_inflight_chunk(chunk_io_request *request)
{
   struct stat st;
   PoolMem inflight_file(PM_FNAME);

   if (request) {
      Mmsg(inflight_file, "%s/%s@%04d", me->working_directory, request->volname, request->chunk);
      pm_strcat(inflight_file, "%inflight");

      Dmsg3(100, "Removing inflight file %s for volume %s, chunk %d\n",
            inflight_file.c_str(), request->volname, request->chunk);

      if (stat(inflight_file.c_str(), &st) != 0) {
         return;
      }

      ::unlink(inflight_file.c_str());
   }

   P(mutex);
   inflight_chunks_++;
   V(mutex);
}

/*
 * Check if a certain chunk is inflight to the backing store.
 */
bool chunked_device::is_inflight_chunk(chunk_io_request *request)
{
   struct stat st;
   PoolMem inflight_file(PM_FNAME);

   Mmsg(inflight_file, "%s/%s@%04d", me->working_directory, request->volname, request->chunk);
   pm_strcat(inflight_file, "%inflight");

   if (stat(inflight_file.c_str(), &st) == 0) {
      return true;
   }

   return false;
}

/*
 * Number of inflight chunks to the backing store.
 */
int chunked_device::nr_inflight_chunks()
{
   int retval = 0;

   P(mutex);
   retval = inflight_chunks_;
   V(mutex);

   return retval;
}

/*
 * Call back function for comparing two chunk_io_requests.
 */
static int compare_chunk_io_request(void *item1, void *item2)
{
   ocbuf_item *ocbuf1 = (ocbuf_item *)item1;
   ocbuf_item *ocbuf2 = (ocbuf_item *)item2;
   chunk_io_request *chunk1 = (chunk_io_request *)ocbuf1->data;
   chunk_io_request *chunk2 = (chunk_io_request *)ocbuf2->data;

   /*
    * Same volume name ?
    */
   if (bstrcmp(chunk1->volname, chunk2->volname)) {
      /*
       * Compare on chunk number.
       */
      if (chunk1->chunk == chunk2->chunk) {
         return 0;
      } else {
         return (chunk1->chunk < chunk2->chunk) ? -1 : 1;
      }
   } else {
      return strcmp(chunk1->volname, chunk2->volname);
   }
}

/*
 * Call back function for updating two chunk_io_requests.
 */
static void update_chunk_io_request(void *item1, void *item2)
{
   chunk_io_request *chunk1 = (chunk_io_request *)item1;
   chunk_io_request *chunk2 = (chunk_io_request *)item2;

   /*
    * See if the new chunk_io_request has more bytes then
    * the chunk_io_request currently on the ordered circular
    * buffer. We can only have multiple chunk_io_requests for
    * the same chunk of a volume when a chunk was not fully
    * filled by one backup Job and a next one writes data to
    * the chunk before its being flushed to backing store. This
    * means all pointers are the same only the wbuflen and the
    * release flag of the chunk_io_request differ. So we only
    * copy those two fields and not the others.
    */
   if (chunk2->buffer == chunk1->buffer &&
       chunk2->wbuflen > chunk1->wbuflen) {
      chunk1->wbuflen = chunk2->wbuflen;
      chunk1->release = chunk2->release;
   }
   chunk2->release = false;
}

/*
 * Enqueue a chunk flush request onto the ordered circular buffer.
 */
bool chunked_device::enqueue_chunk(chunk_io_request *request)
{
   chunk_io_request *new_request,
                    *enqueued_request;

   Dmsg2(100, "Enqueueing chunk %d of volume %s\n", request->chunk, request->volname);

   if (!io_threads_started_) {
      if (!start_io_threads()) {
         return false;
      }
   }

   new_request = (chunk_io_request *)malloc(sizeof(chunk_io_request));
   memset(new_request, 0, sizeof(chunk_io_request));
   new_request->volname = bstrdup(request->volname);
   new_request->chunk = request->chunk;
   new_request->buffer = request->buffer;
   new_request->wbuflen = request->wbuflen;
   new_request->tries = 0;
   new_request->release = request->release;

   Dmsg2(100, "Allocated chunk io request of %d bytes at %p\n", sizeof(chunk_io_request), new_request);

   /*
    * Enqueue the item onto the ordered circular buffer.
    * This returns either the same request as we passed
    * in or the previous flush request for the same chunk.
    */
   enqueued_request = (chunk_io_request *)cb_->enqueue(new_request,
                                                        sizeof(chunk_io_request),
                                                        compare_chunk_io_request,
                                                        update_chunk_io_request,
                                                        false, /* use_reserved_slot */
                                                        false /* no_signal */);

   /*
    * Compare the return value from the enqueue.
    */
   if (enqueued_request && enqueued_request != new_request) {
      free_chunk_io_request(new_request);
   }

   return (enqueued_request) ? true : false;
}

/*
 * Dequeue a chunk flush request from the ordered circular buffer and process it.
 */
bool chunked_device::dequeue_chunk()
{
   char ed1[50];
   struct timeval tv;
   struct timezone tz;
   struct timespec ts;
   bool requeued = false;
   chunk_io_request *new_request;

   /*
    * Loop while we are not done either due to the ordered circular buffer being flushed
    * some fatal error or successfully dequeueing a chunk flush request.
    */
   while (1) {
      /*
       * See if we are in the flushing state then we just return and exit the io-thread.
       */
      if (cb_->is_flushing()) {
         return false;
      }

      /*
       * Calculate the next absolute timeout if we find out there is no work to be done.
       */
      gettimeofday(&tv, &tz);
      ts.tv_nsec = tv.tv_usec * 1000;
      ts.tv_sec = tv.tv_sec + DEFAULT_RECHECK_INTERVAL;

      /*
       * Dequeue the next item from the ordered circular buffer and reserve the slot as we
       * might need to put this item back onto the ordered circular buffer if we fail to
       * flush it to the remote backing store. Also let the dequeue wake up every
       * DEFAULT_RECHECK_INTERVAL seconds to retry failed previous uploads.
       */
      new_request = (chunk_io_request *)cb_->dequeue(true, /* reserve_slot we may need to enqueue the request */
                                                      requeued, /* request is requeued due to failure ? */
                                                      &ts, DEFAULT_RECHECK_INTERVAL);
      if (!new_request) {
         return false;
      }

      Dmsg3(100, "Flushing chunk %d of volume %s by thread %s\n",
            new_request->chunk, new_request->volname,
            edit_pthread(pthread_self(), ed1, sizeof(ed1)));

      if (!flush_remote_chunk(new_request)) {
          chunk_io_request *enqueued_request;

          /*
           * See if we have a maximum number of retries to upload chunks to the backing store
           * and if we have and execeeded those tries for this chunk set the device to read-only
           * so any next write to the device will error out. This should prevent us from hanging
           * the flushing to the backing store on misconfigured devices.
           */
          new_request->tries++;
          if (retries_ > 0 && new_request->tries >= retries_) {
             Mmsg4(errmsg, _("Unable to flush chunk %d of volume %s to backing store after %d tries, setting device %s readonly\n"),
                   new_request->chunk, new_request->volname, new_request->tries, print_name());
             Emsg0(M_ERROR, 0, errmsg);
             readonly_ = true;
             goto bail_out;
          }

         /*
          * We failed to flush the chunk to the backing store
          * so enqueue it again using the reserved slot by dequeue()
          * but don't signal the workers otherwise we would try uploading
          * the same chunk again and again by different io-threads.
          * As we set the requeued flag to the dequeue method on the ordered circular buffer
          * we will not try dequeueing any new item either until a new item is put
          * onto the ordered circular buffer or after the retry interval has expired.
          */
         Dmsg2(100, "Enqueueing chunk %d of volume %s for retry of upload later\n",
               new_request->chunk, new_request->volname);

         /*
          * Enqueue the item onto the ordered circular buffer.
          * This returns either the same request as we passed
          * in or the previous flush request for the same chunk.
          */
         enqueued_request = (chunk_io_request *)cb_->enqueue(new_request,
                                                              sizeof(chunk_io_request),
                                                              compare_chunk_io_request,
                                                              update_chunk_io_request,
                                                              true, /* use_reserved_slot */
                                                              true /* no_signal */);
         /*
          * See if the enqueue succeeded.
          */
         if (!enqueued_request) {
            return false;
         }

         /*
          * Compare the return value from the enqueue against our new_request.
          * If it is different there was already a chunk io request for the
          * same chunk on the ordered circular buffer.
          */
         if (enqueued_request != new_request) {
            free_chunk_io_request(new_request);
         }

         requeued = true;
         continue;
      }

bail_out:
      /*
       * Unreserve the slot on the ordered circular buffer reserved by dequeue().
       */
      cb_->unreserve_slot();

      /*
       * Processed the chunk so clean it up now.
       */
      free_chunk_io_request(new_request);

      return true;
   }
}

/*
 * Internal method for flushing a chunk to the backing store.
 * The retry logic is in the io-threads but if those are not
 * used we give this one try and otherwise drop the chunk and
 * return an IO error to the upper level callers. That way the
 * volume will go into error.
 */
bool chunked_device::flush_chunk(bool release_chunk, bool move_to_next_chunk)
{
   bool retval = false;
   chunk_io_request request;

   /*
    * Calculate in which chunk we are currently.
    */
   request.chunk = current_chunk_->start_offset / current_chunk_->chunk_size;
   request.volname = current_volname_;
   request.buffer = current_chunk_->buffer;
   request.wbuflen = current_chunk_->buflen;
   request.release = release_chunk;

   if (io_threads_) {
      retval = enqueue_chunk(&request);
   } else {
      retval = flush_remote_chunk(&request);
   }

   /*
    * Clear the need flushing flag.
    */
   current_chunk_->need_flushing = false;

   /*
    * Change to the next chunk ?
    */
   if (move_to_next_chunk) {
      /*
       * If we enqueued the data we need to allocate a new buffer.
       */
      if (io_threads_) {
         current_chunk_->buffer = allocate_chunkbuffer();
      }
      current_chunk_->start_offset += current_chunk_->chunk_size;
      current_chunk_->end_offset = current_chunk_->start_offset + (current_chunk_->chunk_size - 1);
      current_chunk_->buflen = 0;
   } else {
      /*
       * If we enqueued the data we need to allocate a new buffer.
       */
      if (release_chunk && io_threads_) {
         current_chunk_->buffer = NULL;
      }
   }

   if (!retval) {
      Dmsg1(100, "%s", errmsg);
   }

   return retval;
}

/*
 * Internal method for reading a chunk from the backing store.
 */
bool chunked_device::read_chunk()
{
   chunk_io_request request;

   /*
    * Calculate in which chunk we are currently.
    */
   request.chunk = current_chunk_->start_offset / current_chunk_->chunk_size;
   request.volname = current_volname_;
   request.buffer = current_chunk_->buffer;
   request.wbuflen = current_chunk_->chunk_size;
   request.rbuflen = &current_chunk_->buflen;
   request.release = false;

   current_chunk_->end_offset = current_chunk_->start_offset + (current_chunk_->chunk_size - 1);

   if (!read_remote_chunk(&request)) {
      /*
       * If the chunk doesn't exist on the backing store it has a size of 0 bytes.
       */
      current_chunk_->buflen = 0;
      return false;
   }

   return true;
}

/*
 * Setup a chunked volume for reading or writing.
 */
int chunked_device::setup_chunk(const char *pathname, int flags, int mode)
{
   /*
    * If device is (re)opened and we are put into readonly mode because
    * of problems flushing chunks to the backing store we return EROFS
    * to the upper layers.
    */
   if ((flags & O_RDWR) && readonly_) {
      dev_errno = EROFS;
      return -1;
   }

   if (!current_chunk_) {
      current_chunk_ = (chunk_descriptor *)malloc(sizeof(chunk_descriptor));
      memset(current_chunk_, 0, sizeof(chunk_descriptor));
      if (chunk_size_ > DEFAULT_CHUNK_SIZE) {
         current_chunk_->chunk_size = chunk_size_;
      } else {
         current_chunk_->chunk_size = DEFAULT_CHUNK_SIZE;
      }
      current_chunk_->start_offset = -1;
      current_chunk_->end_offset = -1;
   }

   /*
    * Reopen of a device.
    */
   if (current_chunk_->opened) {
      /*
       * Invalidate chunk.
       */
      current_chunk_->buflen = 0;
      current_chunk_->start_offset = -1;
      current_chunk_->end_offset = -1;
   }

   if (flags & O_RDWR) {
      current_chunk_->writing = true;
   }

   current_chunk_->opened = true;
   current_chunk_->chunk_setup = false;

   /*
    * We need to limit the maximum size of a chunked volume to MAX_CHUNKS * chunk_size).
    */
   if (max_volume_size == 0 || max_volume_size > (uint64_t)(MAX_CHUNKS * current_chunk_->chunk_size)) {
      max_volume_size = MAX_CHUNKS * current_chunk_->chunk_size;
   }

   /*
    * On open set begin offset to 0.
    */
   offset_ = 0;

   /*
    * On open we are no longer at the End of the Media.
    */
   end_of_media_ = false;

   /*
    * Keep track of the volume currently mounted.
    */
   if (current_volname_) {
      free(current_volname_);
   }

   current_volname_ = bstrdup(getVolCatName());

   return 0;
}

/*
 * Read a chunked volume.
 */
ssize_t chunked_device::read_chunked(int fd, void *buffer, size_t count)
{
   ssize_t retval = 0;

   if (current_chunk_->opened) {
      ssize_t wanted_offset;
      ssize_t bytes_left;

      /*
       * Shortcut logic see if end_of_media_ is set then we are at the End of the Media
       */
      if (end_of_media_) {
         goto bail_out;
      }

      /*
       * If we are starting reading without the chunk being setup it means we
       * are start reading at the beginning of the file otherwise the d_lseek method
       * would have read in the correct chunk.
       */
      if (!current_chunk_->chunk_setup) {
         current_chunk_->start_offset = 0;

         /*
          * See if we have to allocate a new buffer.
          */
         if (!current_chunk_->buffer) {
            current_chunk_->buffer = allocate_chunkbuffer();
         }

         if (!read_chunk()) {
            retval = -1;
            goto bail_out;
         }
         current_chunk_->chunk_setup = true;
      }

      /*
       * See if we can fulfill the wanted read from the current chunk.
       */
      if (current_chunk_->start_offset <= offset_ &&
          current_chunk_->end_offset >= (boffset_t)((offset_ + count) - 1)) {
         wanted_offset = (offset_ % current_chunk_->chunk_size);

         bytes_left = MIN((ssize_t)count, (current_chunk_->buflen - wanted_offset));
         Dmsg2(200, "Reading %d bytes at offset %d from chunk buffer\n", bytes_left, wanted_offset);

         if (bytes_left < 0) {
            retval = -1;
            goto bail_out;
         }

         if (bytes_left > 0) {
            memcpy(buffer, current_chunk_->buffer + wanted_offset, bytes_left);
         }
         offset_ += bytes_left;
         retval = bytes_left;
         goto bail_out;
      } else {
         ssize_t offset = 0;

         /*
          * We cannot fulfill the read from the current chunk, see how much
          * is available and return that and see if by reading the next chunk
          * we can fulfill the whole read. When then we still have not filled
          * the whole buffer we keep on reading any next chunk until none are
          * left and we have reached End Of Media.
          */
         while (retval < (ssize_t)count) {
            /*
             * See how much is left in this chunk.
             */
            if (offset_ < current_chunk_->end_offset) {
               wanted_offset = (offset_ % current_chunk_->chunk_size);
               bytes_left = MIN((ssize_t)(count - offset),
                                (ssize_t)(current_chunk_->buflen - wanted_offset));

               if (bytes_left > 0) {
                  Dmsg2(200, "Reading %d bytes at offset %d from chunk buffer\n", bytes_left, wanted_offset);

                  memcpy(((char *)buffer + offset), current_chunk_->buffer + wanted_offset, bytes_left);
                  offset_ += bytes_left;
                  offset += bytes_left;
                  retval += bytes_left;
               }
            }

            /*
             * Read in the next chunk.
             */
            current_chunk_->start_offset += current_chunk_->chunk_size;
            if (!read_chunk()) {
               switch (dev_errno) {
               case EIO:
                  /*
                   * If the are no more chunks to read we return only the bytes available.
                   * We also set end_of_media_ as we are at the end of media.
                   */
                  end_of_media_ = true;
                  goto bail_out;
               default:
                  retval = -1;
                  goto bail_out;
               }
            } else {
               /*
                * Calculate how much data we can read from the just freshly read chunk.
                */
               bytes_left = MIN((ssize_t)(count - offset),
                                (ssize_t)(current_chunk_->buflen));

               if (bytes_left > 0) {
                  Dmsg2(200, "Reading %d bytes at offset %d from chunk buffer\n", bytes_left, 0);

                  memcpy(((char *)buffer + offset), current_chunk_->buffer, bytes_left);
                  offset_ += bytes_left;
                  offset += bytes_left;
                  retval += bytes_left;
               }
            }
         }
      }
   } else {
      errno = EBADF;
      retval = -1;
   }

bail_out:
   return retval;
}

/*
 * Write a chunked volume.
 */
ssize_t chunked_device::write_chunked(int fd, const void *buffer, size_t count)
{
   ssize_t retval = 0;

   /*
    * If we are put into readonly mode because of problems flushing chunks to the
    * backing store we return EIO to the upper layers.
    */
   if (readonly_) {
      errno = EIO;
      retval = -1;
      goto bail_out;
   }

   if (current_chunk_->opened) {
      ssize_t wanted_offset;

      /*
       * If we are starting writing without the chunk being setup it means we
       * are start writing to an empty file because otherwise the d_lseek method
       * would have read in the correct chunk.
       */
      if (!current_chunk_->chunk_setup) {
         current_chunk_->start_offset = 0;
         current_chunk_->end_offset = (current_chunk_->chunk_size - 1);
         current_chunk_->buflen = 0;
         current_chunk_->chunk_setup = true;

         /*
          * See if we have to allocate a new buffer.
          */
         if (!current_chunk_->buffer) {
            current_chunk_->buffer = allocate_chunkbuffer();
         }
      }

      /*
       * See if we can write the whole data inside the current chunk.
       */
      if (current_chunk_->start_offset <= offset_ &&
          current_chunk_->end_offset >= (boffset_t)((offset_ + count) - 1)) {

         wanted_offset = (offset_ % current_chunk_->chunk_size);

         Dmsg2(200, "Writing %d bytes at offset %d in chunk buffer\n", count, wanted_offset);

         memcpy(current_chunk_->buffer + wanted_offset, buffer, count);

         offset_ += count;
         if ((wanted_offset + count) > current_chunk_->buflen) {
            current_chunk_->buflen = wanted_offset + count;
         }
         current_chunk_->need_flushing = true;
         retval = count;
      } else {
         ssize_t bytes_left;
         ssize_t offset = 0;

         /*
          * Things don't fit so first write as many bytes as can be written into
          * the current chunk and then flush it and write the next bytes into the
          * next chunk. When then things still don't fit loop until all bytes are
          * written.
          */
         while (retval < (ssize_t)count) {
            /*
             * See how much is left in this chunk.
             */
            if (offset_ < current_chunk_->end_offset) {
               wanted_offset = (offset_ % current_chunk_->chunk_size);
               bytes_left = MIN((ssize_t)(count - offset),
                                (ssize_t)((current_chunk_->end_offset - (current_chunk_->start_offset + wanted_offset)) + 1));

               if (bytes_left > 0) {
                  Dmsg2(200, "Writing %d bytes at offset %d in chunk buffer\n", bytes_left, wanted_offset);

                  memcpy(current_chunk_->buffer + wanted_offset, ((char *)buffer + offset), bytes_left);
                  offset_ += bytes_left;
                  if ((wanted_offset + bytes_left) > current_chunk_->buflen) {
                     current_chunk_->buflen = wanted_offset + bytes_left;
                  }
                  current_chunk_->need_flushing = true;
                  offset += bytes_left;
                  retval += bytes_left;
               }
            }

            /*
             * Flush out the current chunk.
             */
            if (!flush_chunk(true /* release */, true /* move_to_next_chunk */)) {
               retval = -1;
               goto bail_out;
            }

            /*
             * Calculate how much data we can fit into the just freshly created chunk.
             */
            bytes_left = MIN((ssize_t)(count - offset),
                             (ssize_t)((current_chunk_->end_offset - current_chunk_->start_offset) + 1));
            if (bytes_left > 0) {
               Dmsg2(200, "Writing %d bytes at offset %d in chunk buffer\n", bytes_left, 0);

               memcpy(current_chunk_->buffer, ((char *)buffer + offset), bytes_left);
               current_chunk_->buflen = bytes_left;
               current_chunk_->need_flushing = true;
               offset_ += bytes_left;
               offset += bytes_left;
               retval += bytes_left;
            }
         }
      }
   } else {
      errno = EBADF;
      retval = -1;
   }

bail_out:
   return retval;
}

/*
 * Close a chunked volume.
 */
int chunked_device::close_chunk()
{
   int retval = -1;

   if (current_chunk_->opened) {
      if (current_chunk_->need_flushing) {
         if (flush_chunk(true /* release */, false /* move_to_next_chunk */)) {
            retval = 0;
         } else {
            dev_errno = EIO;
         }
      }

      /*
       * Invalidate chunk.
       */
      current_chunk_->writing = false;
      current_chunk_->opened = false;
      current_chunk_->chunk_setup = false;
      current_chunk_->buflen = 0;
      current_chunk_->start_offset = -1;
      current_chunk_->end_offset = -1;
   } else {
      errno = EBADF;
   }

   return retval;
}

/*
 * Truncate a chunked volume.
 */
bool chunked_device::truncate_chunked_volume(DeviceControlRecord *dcr)
{
   if (current_chunk_->opened) {
      if (!truncate_remote_chunked_volume(dcr)) {
         return false;
      }

      /*
       * Reinitialize the initial chunk.
       */
      current_chunk_->start_offset = 0;
      current_chunk_->end_offset = (current_chunk_->chunk_size - 1);
      current_chunk_->buflen = 0;
      current_chunk_->chunk_setup = true;
      current_chunk_->need_flushing = false;

      /*
       * Reinitialize the volume name on a relabel we could get a new name.
       */
      if (current_volname_) {
         free(current_volname_);
      }

      current_volname_ = bstrdup(getVolCatName());
   }

   return true;
}

static int compare_volume_name(void *item1, void *item2)
{
   const char *volname = (const char *)item2;
   chunk_io_request *request = (chunk_io_request *)item1;

   return strcmp(request->volname, volname);
}

/*
 * Get the current size of a volume.
 */
ssize_t chunked_device::chunked_volume_size()
{
   /*
    * See if we are using io-threads or not and the ordered circbuf is created.
    * We try to make sure that nothing of the volume being requested is still inflight as then
    * the chunked_remote_volume_size() method will fail to determine the size of the data as
    * its not fully stored on the backing store yet.
    */
   if (io_threads_ > 0 && cb_) {
      while (1) {
         if (!cb_->empty()) {
            chunk_io_request *request;

            /*
             * Peek on the ordered circular queue if there are any pending IO-requests
             * for this volume. If there are use that as the indication of the size of
             * the volume and don't contact the remote storage as there is still data
             * inflight and as such we need to look at the last chunk that is still not
             * uploaded of the volume.
             */
            request = (chunk_io_request *)cb_->peek(PEEK_LAST, current_volname_, compare_volume_name);
            if (request) {
               ssize_t retval;

               /*
                * Calculate the size of the volume based on the last chunk inflight.
                */
               retval = (request->chunk * current_chunk_->chunk_size) + request->wbuflen;

               /*
                * The peek method gives us a cloned chunk_io_request with pointers to
                * the original chunk_io_request. We just need to free the structure not
                * the content so we call free() here and not free_chunk_io_request() !
                */
               free(request);

               return retval;
            }
         }

         /*
          * Chunk doesn't seem to be on the ordered circular buffer.
          * Make sure there is also nothing inflight to the backing store anymore.
          */
         if (nr_inflight_chunks() > 0) {
            uint8_t retries = INFLIGHT_RETRIES;

            /*
             * There seem to be inflight chunks to the backing store so busy wait until there
             * is nothing inflight anymore. The chunks either get uploaded and as such we
             * can just get the volume size from the backing store or it gets put back onto
             * the ordered circular list and then we can pick it up by retrying the PEEK_LAST
             * on the ordered circular list.
             */
            do {
               bmicrosleep(INFLIGT_RETRY_TIME, 0);
            } while (nr_inflight_chunks() > 0 && --retries > 0);

            /*
             * If we ran out of retries we most likely encountered a stale inflight file.
             */
            if (!retries) {
               clear_inflight_chunk(NULL);
               break;
            }

            /*
             * Do a new try on the ordered circular list to get the last pending IO-request
             * for the volume we are trying to get the size of.
             */
            continue;
         } else {
            /*
             * Its not on the ordered circular list and not inflight so it must be on the
             * backing store so we break the loop and try to get the volume size from the
             * chunks available on the backing store.
             */
            break;
         }
      }
   }

   /*
    * Get the actual length by contacting the remote backing store.
    */
   return chunked_remote_volume_size();
}

static int clone_io_request(void *item1, void *item2)
{
   chunk_io_request *src = (chunk_io_request *)item1;
   chunk_io_request *dst = (chunk_io_request *)item2;

   if (bstrcmp(src->volname, dst->volname) && src->chunk == dst->chunk) {
      memcpy(dst->buffer, src->buffer, src->wbuflen);
      *dst->rbuflen = src->wbuflen;

      /*
       * Cloning succeeded.
       */
      return 0;
   }

   /*
    * Not the right volname or chunk.
    */
   return -1;
}

/*
 * Make sure we have the right chunk in memory.
 */
bool chunked_device::load_chunk()
{
   boffset_t start_offset;

   start_offset = (offset_ / current_chunk_->chunk_size) * current_chunk_->chunk_size;

   /*
    * See if we have to allocate a new buffer.
    */
   if (!current_chunk_->buffer) {
      current_chunk_->buffer = allocate_chunkbuffer();
   }

   /*
    * If the wrong chunk is loaded populate the chunk buffer with the right data.
    */
   if (start_offset != current_chunk_->start_offset) {
      current_chunk_->buflen = 0;
      current_chunk_->start_offset = start_offset;

      /*
       * See if we are using io-threads or not and the ordered circbuf is created.
       * We try to make sure that nothing of the volume being requested is still inflight as then
       * the read_chunk() method will fail to read the data as its not stored on the backing
       * store yet.
       */
      if (io_threads_ > 0 && cb_) {
         chunk_io_request request;

         request.chunk = current_chunk_->start_offset / current_chunk_->chunk_size;
         request.volname = current_volname_;
         request.buffer = current_chunk_->buffer;
         request.rbuflen = &current_chunk_->buflen;

         while (1) {
            if (!cb_->empty()) {
               /*
                * Peek on the ordered circular queue and clone the data which is infligt back to the
                * current chunk buffer. When we are able to clone the data the peek will return the
                * address of the request structure it used for the clone operation. When nothing could
                * be cloned it will return NULL. If data is cloned we use that and skip the call to
                * read the data from the backing store as that will not have the latest data anyway.
                */
               if (cb_->peek(PEEK_CLONE, &request, clone_io_request) == &request) {
                  goto bail_out;
               }
            }

            /*
             * Chunk doesn't seem to be on the ordered circular buffer.
             * Make sure its also not inflight to the backing store.
             */
            if (is_inflight_chunk(&request)) {
               uint8_t retries = INFLIGHT_RETRIES;

               /*
                * Chunk seems to be inflight busy wait until its no longer.
                * It either gets uploaded and as such we can just read it from the backing store
                * again or it gets put back onto the ordered circular list and then we can pick
                * it up by retrying the PEEK_CLONE on the ordered circular list.
                */
               do {
                  bmicrosleep(INFLIGT_RETRY_TIME, 0);
               } while (is_inflight_chunk(&request) && --retries > 0);

               /*
                * If we ran out of retries we most likely encountered a stale inflight file.
                */
               if (!retries) {
                  clear_inflight_chunk(&request);
                  break;
               }

               /*
                * Do a new try to clone the data from the ordered circular list.
                */
               continue;
            } else {
               /*
                * Its not on the ordered circular list and not inflight so it must be on the
                * backing store so we break the loop and try to read the chunk from the backing store.
                */
               break;
            }
         }
      }

      /*
       * Read the chunk from the backing store.
       */
      if (!read_chunk()) {
         switch (dev_errno) {
         case EIO:
            if (current_chunk_->writing) {
               current_chunk_->end_offset = start_offset + (current_chunk_->chunk_size - 1);
            }
            break;
         default:
            return false;
         }
      }
   }

bail_out:
   current_chunk_->chunk_setup = true;

   return true;
}

static int list_io_request(void *request, void *data)
{
   chunk_io_request *io_request = (chunk_io_request *)request;
   bsdDevStatTrig *dst = (bsdDevStatTrig *)data;
   PoolMem status(PM_MESSAGE);

   status.bsprintf("   /%s/%04d - %ld\n", io_request->volname, io_request->chunk, io_request->wbuflen);
   dst->status_length = pm_strcat(dst->status, status.c_str());

   return 0;
}

/*
 * Return specific device status information.
 */
bool chunked_device::device_status(bsdDevStatTrig *dst)
{
   /*
    * See if we are using io-threads or not and the ordered circbuf is created and not empty.
    */
   dst->status_length = 0;
   if (io_threads_ > 0 && cb_) {
      if (!cb_->empty()) {
         dst->status_length = pm_strcpy(dst->status, _("Pending IO flush requests:\n"));

         /*
          * Peek on the ordered circular queue and list all pending requests.
          */
         cb_->peek(PEEK_LIST, dst, list_io_request);
      } else {
         dst->status_length = pm_strcpy(dst->status, _("No Pending IO flush requests\n"));
      }
   }

   return (dst->status_length > 0);
}

chunked_device::~chunked_device()
{
   if (thread_ids_) {
      stop_threads();
   }

   if (cb_) {
      /*
       * If there is any work on the ordered circular buffer remove it.
       */
      if (!cb_->empty()) {
         chunk_io_request *request;
         do {
            request = (chunk_io_request *)cb_->dequeue();
            if (request) {
               request->release = true;
               free_chunk_io_request(request);
            }
         } while (!cb_->empty());
      }

      delete cb_;
      cb_ = NULL;
   }

   if (current_chunk_) {
      if (current_chunk_->buffer) {
         free_chunkbuffer(current_chunk_->buffer);
      }
      free(current_chunk_);
      current_chunk_ = NULL;
   }

   if (current_volname_) {
      free(current_volname_);
   }
}

chunked_device::chunked_device()
{
   current_volname_ = NULL;
   current_chunk_ = NULL;
   io_threads_ = 0;
   io_slots_ = 0;
   retries_ = 0;
   chunk_size_ = 0;
   io_threads_started_ = false;
   end_of_media_ = false;
   readonly_ = false;
   inflight_chunks_ = 0;
   cb_ = NULL;
   io_threads_ = 0;
   chunk_size_ = 0;
   offset_ = 0;
   use_mmap_ = false;
}
#endif /* HAVE_DROPLET */