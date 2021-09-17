/* Copyright libuv project contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "uv.h"
#include "task.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#if !defined(_WIN32)
# include <sys/types.h>
# include <sys/time.h>
#endif

/* This test simulates how Electron does node integration. */

/*
 * The node_loop acts as the event loop of Node.js running in the main thread.
 */
static uv_loop_t node_loop;
/*
 * The fake_gui_loop acts as the GUI event loop running in the main thread.
 */
static uv_loop_t fake_gui_loop;
/*
 * The embed_thread is a background thread where the backend fd of node_loop is
 * being polled.
 */
static uv_thread_t embed_thread;
/*
 * The embed_sem is used start/pause the polling in embed_thread.
 */
static uv_sem_t embed_sem;
/*
 * The wake_up_gui_async is used for waking up the fake_gui_loop.
 */
static uv_async_t wake_up_gui_async;
/*
 * The node_dummy_async does nothing but to make the node_loop has timeout of
 * INFINITE, otherwise the embed_thread will have nothing to wait and goes into
 * busy loop.
 */
static uv_async_t node_dummy_async;
/*
 * The embed_closed is used to notify the exit of Node.js.
 */
static volatile int embed_closed;
/*
 * The node_loop_timeout is used to pass the timeout of backend fd between
 * the embed_thread and the main thread.
 */
static int node_loop_timeout;


/*
 * The node_timer is created for the node_loop, creating the timer is supposed
 * to create an IO event node_loop, which then interrupt the polling in in the
 * embed_thread, which then wakes up the fake_gui_loop.
 */
static uv_timer_t node_timer;
static int node_timer_called;


#if defined(_WIN32)
static void embed_thread_poll_win(HANDLE iocp, int timeout) {
  DWORD bytes;
  ULONG_PTR key;
  OVERLAPPED* overlapped;

  GetQueuedCompletionStatus(iocp,
                            &bytes,
                            &key,
                            &overlapped,
                            timeout >= 0 ? timeout : INFINITE);

  /* Give the event back so the loop can deal with it. */
  if (overlapped != NULL)
    PostQueuedCompletionStatus(iocp,
                               bytes,
                               key,
                               overlapped);
}
#else
static void embed_thread_poll_unix(int fd, int timeout) {
  int r;
  do {
    struct timeval tv;
    if (timeout >= 0) {
      tv.tv_sec = timeout / 1000;
      tv.tv_usec = (timeout % 1000) * 1000;
    }
    fd_set readset;
    FD_ZERO(&readset);
    FD_SET(fd, &readset);
    /* Use select instead of epoll_wait/kqueue, so when there is IO event
     * we only get notified and won't remove the event from queue, which
     * would make libuv unable to emit callback for the event. */
    r = select(fd + 1, &readset, NULL, NULL, timeout >= 0 ? &tv : NULL);
  } while (r == -1 && errno == EINTR);
}
#endif /* !_WIN32 */


static void embed_thread_runner(void* arg) {
  while (1) {
    uv_sem_wait(&embed_sem);
    if (embed_closed)
      break;

#if defined(_WIN32)
    embed_thread_poll_win(node_loop.iocp, node_loop_timeout);
#else
    embed_thread_poll_unix(uv_backend_fd(&node_loop), node_loop_timeout);
#endif

    uv_async_send(&wake_up_gui_async);
  }
}


static void embed_cb(uv_async_t* async) {
  /* Run uv tasks in node loop */
  uv_run(&node_loop, UV_RUN_NOWAIT);

  /* Tell embed thread to continue polling */
  node_loop_timeout = uv_backend_timeout(&node_loop);
  uv_sem_post(&embed_sem);
}


static void node_timer_cb(uv_timer_t* timer) {
  node_timer_called++;
  embed_closed = 1;

  uv_close((uv_handle_t*) &wake_up_gui_async, NULL);
  uv_close((uv_handle_t*) &node_dummy_async, NULL);
}


static void init_loops(void) {
  ASSERT_EQ(0, uv_loop_init(&node_loop));
  ASSERT_EQ(0, uv_loop_init(&fake_gui_loop));

  node_timer_called = 0;
  embed_closed = 0;

  uv_async_init(&fake_gui_loop, &wake_up_gui_async, embed_cb);

  /* Create a dummy async for node loop otherwise backend timeout will
     always be 0 */
  uv_async_init(&node_loop, &node_dummy_async, embed_cb);

  /* Start worker that will poll main loop and interrupt fake gui loop */
  uv_sem_init(&embed_sem, 0);
  uv_thread_create(&embed_thread, embed_thread_runner, NULL);
}


static void run_loop(void) {
  /* Run node_loop for once to give things a chance to initialize.*/
  embed_cb(&wake_up_gui_async);

  /* Run fake_gui_loop */
  uv_run(&fake_gui_loop, UV_RUN_DEFAULT);

  /* When fake_gui_loop is interruptted, the purpose of test is achieved. */
  uv_thread_join(&embed_thread);
  uv_sem_destroy(&embed_sem);
  uv_loop_close(&fake_gui_loop);
  uv_loop_close(&node_loop);
}


TEST_IMPL(embed) {
  init_loops();

  /* Start timer in node_loop. */
  uv_timer_init(&node_loop, &node_timer);
  uv_timer_start(&node_timer, node_timer_cb, 250, 0);

  run_loop();
  ASSERT_EQ(node_timer_called, 1);

  return 0;
}


/*
 * The gui_timer is used to run a task in fake_gui_loop, which simulates how
 * Electron runs user JavaScript in the gui event loop.
 */
static uv_timer_t gui_timer;


/*
 * Simulates user running setTimeout() in JavaScript, which is supposed to
 * interrupt the polling in embed_thread.
 */
static void gui_run_settimeout(uv_timer_t* timer) {
  uv_timer_init(&node_loop, &node_timer);
  uv_timer_start(&node_timer, node_timer_cb, 250, 0);
}


TEST_IMPL(embed_with_timer) {
  init_loops();

  /* Interrupt embed polling when watcher is changed in node_loop, e.g. user
   * calls setTimeout or fs.readFile. */
  ASSERT_EQ(0, uv_loop_configure(&node_loop, UV_LOOP_INTERRUPT_ON_IO_CHANGE));

  /* Initialize the gui_timer, the timeout value does not matter in test. */
  uv_timer_init(&fake_gui_loop, &gui_timer);
  uv_timer_start(&gui_timer, gui_run_settimeout, 100, 0);

  run_loop();
  ASSERT_EQ(node_timer_called, 1);
  return 0;
}


/*
 * Simulates user running fs.readdir() in JavaScript, which is supposed to
 * interrupt the polling in embed_thread.
 */
static uv_fs_t opendir_req;
static uv_fs_t readdir_req;
static uv_fs_t closedir_req;
static uv_dirent_t dirents[1];
static int fs_readdir_cb_called;


static void gui_run_fs_readdir_cb(uv_fs_t* req) {
  fs_readdir_cb_called = 1;
  uv_fs_req_cleanup(&readdir_req);
  uv_fs_closedir(&node_loop, &closedir_req, req->ptr, NULL);
  uv_fs_req_cleanup(&closedir_req);
  uv_fs_req_cleanup(&opendir_req);
  /* By the time this callback is called, we are sure the node integration is
   * working and get ready to end the exit. */
  uv_timer_init(&node_loop, &node_timer);
  uv_timer_start(&node_timer, node_timer_cb, 0, 0);
}


static void gui_run_fs_opendir_cb(uv_fs_t* req) {
  uv_dir_t* dir;
  dir = req->ptr;
  dir->dirents = dirents;
  dir->nentries = ARRAY_SIZE(dirents);
  uv_fs_readdir(&node_loop, &readdir_req, dir, gui_run_fs_readdir_cb);
  uv_fs_req_cleanup(req);
}


static void gui_run_fs_opendir(uv_timer_t* timer) {
  uv_fs_opendir(&node_loop, &opendir_req, ".", gui_run_fs_opendir_cb);
}


/*
 * Similar to the embed_with_timer test above, but tests whether the polling can
 * be interruptted when a fd is added to watcher list.
 */
TEST_IMPL(embed_with_fs_opendir) {
  fs_readdir_cb_called = 0;
  init_loops();
  uv_timer_init(&fake_gui_loop, &gui_timer);
  uv_timer_start(&gui_timer, gui_run_fs_opendir, 100, 0);
  run_loop();
  ASSERT_EQ(fs_readdir_cb_called, 1);
  return 0;
}
