/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "src/core/lib/iomgr/port.h"

#ifdef GRPC_WINSOCK_SOCKET

#include "src/core/lib/iomgr/sockaddr_windows.h"

#include <grpc/slice_buffer.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/log_windows.h>
#include <grpc/support/useful.h>

#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/iomgr/iocp_windows.h"
#include "src/core/lib/iomgr/sockaddr.h"
#include "src/core/lib/iomgr/sockaddr_utils.h"
#include "src/core/lib/iomgr/socket_windows.h"
#include "src/core/lib/iomgr/tcp_client.h"
#include "src/core/lib/iomgr/tcp_windows.h"
#include "src/core/lib/iomgr/timer.h"

typedef struct {
  grpc_closure *on_done;
  gpr_mu mu;
  grpc_winsocket *socket;
  gpr_timespec deadline;
  grpc_timer alarm;
  grpc_closure on_alarm;
  char *addr_name;
  int refs;
  grpc_closure on_connect;
  grpc_endpoint **endpoint;
  grpc_resource_quota *resource_quota;
} async_connect;

static void async_connect_unlock_and_cleanup(grpc_exec_ctx *exec_ctx,
                                             async_connect *ac,
                                             grpc_winsocket *socket) {
  int done = (--ac->refs == 0);
  gpr_mu_unlock(&ac->mu);
  if (done) {
    grpc_resource_quota_unref_internal(exec_ctx, ac->resource_quota);
    gpr_mu_destroy(&ac->mu);
    gpr_free(ac->addr_name);
    gpr_free(ac);
  }
  if (socket != NULL) grpc_winsocket_destroy(socket);
}

static void on_alarm(grpc_exec_ctx *exec_ctx, void *acp, grpc_error *error) {
  async_connect *ac = acp;
  gpr_mu_lock(&ac->mu);
  grpc_winsocket *socket = ac->socket;
  ac->socket = NULL;
  if (socket != NULL) {
    grpc_winsocket_shutdown(socket);
  }
  async_connect_unlock_and_cleanup(exec_ctx, ac, socket);
}

static void on_connect(grpc_exec_ctx *exec_ctx, void *acp, grpc_error *error) {
  async_connect *ac = acp;
  grpc_endpoint **ep = ac->endpoint;
  GPR_ASSERT(*ep == NULL);
  grpc_closure *on_done = ac->on_done;

  GRPC_ERROR_REF(error);

  gpr_mu_lock(&ac->mu);
  grpc_winsocket *socket = ac->socket;
  ac->socket = NULL;
  gpr_mu_unlock(&ac->mu);

  grpc_timer_cancel(exec_ctx, &ac->alarm);

  gpr_mu_lock(&ac->mu);

  if (error == GRPC_ERROR_NONE) {
    if (socket != NULL) {
      DWORD transfered_bytes = 0;
      DWORD flags;
      BOOL wsa_success =
          WSAGetOverlappedResult(socket->socket, &socket->write_info.overlapped,
                                 &transfered_bytes, FALSE, &flags);
      GPR_ASSERT(transfered_bytes == 0);
      if (!wsa_success) {
        error = GRPC_WSA_ERROR(WSAGetLastError(), "ConnectEx");
      } else {
        *ep = grpc_tcp_create(socket, ac->resource_quota, ac->addr_name);
        socket = NULL;
      }
    } else {
      error = GRPC_ERROR_CREATE("socket is null");
    }
  }

  async_connect_unlock_and_cleanup(exec_ctx, ac, socket);
  /* If the connection was aborted, the callback was already called when
     the deadline was met. */
  grpc_closure_sched(exec_ctx, on_done, error);
}

/* Tries to issue one async connection, then schedules both an IOCP
   notification request for the connection, and one timeout alert. */
void grpc_tcp_client_connect(grpc_exec_ctx *exec_ctx, grpc_closure *on_done,
                             grpc_endpoint **endpoint,
                             grpc_pollset_set *interested_parties,
                             const grpc_channel_args *channel_args,
                             const grpc_resolved_address *addr,
                             gpr_timespec deadline) {
  SOCKET sock = INVALID_SOCKET;
  BOOL success;
  int status;
  grpc_resolved_address addr6_v4mapped;
  grpc_resolved_address local_address;
  async_connect *ac;
  grpc_winsocket *socket = NULL;
  LPFN_CONNECTEX ConnectEx;
  GUID guid = WSAID_CONNECTEX;
  DWORD ioctl_num_bytes;
  grpc_winsocket_callback_info *info;
  grpc_error *error = GRPC_ERROR_NONE;

  grpc_resource_quota *resource_quota = grpc_resource_quota_create(NULL);
  if (channel_args != NULL) {
    for (size_t i = 0; i < channel_args->num_args; i++) {
      if (0 == strcmp(channel_args->args[i].key, GRPC_ARG_RESOURCE_QUOTA)) {
        grpc_resource_quota_unref_internal(exec_ctx, resource_quota);
        resource_quota = grpc_resource_quota_ref_internal(
            channel_args->args[i].value.pointer.p);
      }
    }
  }

  *endpoint = NULL;

  /* Use dualstack sockets where available. */
  if (grpc_sockaddr_to_v4mapped(addr, &addr6_v4mapped)) {
    addr = &addr6_v4mapped;
  }

  sock = WSASocket(AF_INET6, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
                   WSA_FLAG_OVERLAPPED);
  if (sock == INVALID_SOCKET) {
    error = GRPC_WSA_ERROR(WSAGetLastError(), "WSASocket");
    goto failure;
  }

  error = grpc_tcp_prepare_socket(sock);
  if (error != GRPC_ERROR_NONE) {
    goto failure;
  }

  /* Grab the function pointer for ConnectEx for that specific socket.
     It may change depending on the interface. */
  status =
      WSAIoctl(sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid),
               &ConnectEx, sizeof(ConnectEx), &ioctl_num_bytes, NULL, NULL);

  if (status != 0) {
    error = GRPC_WSA_ERROR(WSAGetLastError(),
                           "WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER)");
    goto failure;
  }

  grpc_sockaddr_make_wildcard6(0, &local_address);

  status = bind(sock, (struct sockaddr *)&local_address.addr,
                (int)local_address.len);
  if (status != 0) {
    error = GRPC_WSA_ERROR(WSAGetLastError(), "bind");
    goto failure;
  }

  socket = grpc_winsocket_create(sock, "client");
  info = &socket->write_info;
  success = ConnectEx(sock, (struct sockaddr *)&addr->addr, (int)addr->len,
                      NULL, 0, NULL, &info->overlapped);

  /* It wouldn't be unusual to get a success immediately. But we'll still get
     an IOCP notification, so let's ignore it. */
  if (!success) {
    int last_error = WSAGetLastError();
    if (last_error != ERROR_IO_PENDING) {
      error = GRPC_WSA_ERROR(last_error, "ConnectEx");
      goto failure;
    }
  }

  ac = gpr_malloc(sizeof(async_connect));
  ac->on_done = on_done;
  ac->socket = socket;
  gpr_mu_init(&ac->mu);
  ac->refs = 2;
  ac->addr_name = grpc_sockaddr_to_uri(addr);
  ac->endpoint = endpoint;
  ac->resource_quota = resource_quota;
  grpc_closure_init(&ac->on_connect, on_connect, ac, grpc_schedule_on_exec_ctx);

  grpc_closure_init(&ac->on_alarm, on_alarm, ac, grpc_schedule_on_exec_ctx);
  grpc_timer_init(exec_ctx, &ac->alarm, deadline, &ac->on_alarm,
                  gpr_now(GPR_CLOCK_MONOTONIC));
  grpc_socket_notify_on_write(exec_ctx, socket, &ac->on_connect);
  return;

failure:
  GPR_ASSERT(error != GRPC_ERROR_NONE);
  char *target_uri = grpc_sockaddr_to_uri(addr);
  grpc_error *final_error = grpc_error_set_str(
      GRPC_ERROR_CREATE_REFERENCING("Failed to connect", &error, 1),
      GRPC_ERROR_STR_TARGET_ADDRESS, target_uri);
  GRPC_ERROR_UNREF(error);
  if (socket != NULL) {
    grpc_winsocket_destroy(socket);
  } else if (sock != INVALID_SOCKET) {
    closesocket(sock);
  }
  grpc_resource_quota_unref_internal(exec_ctx, resource_quota);
  grpc_closure_sched(exec_ctx, on_done, final_error);
}

#endif /* GRPC_WINSOCK_SOCKET */
