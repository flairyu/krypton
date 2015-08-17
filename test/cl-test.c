/*
 * Copyright (c) 2015 Cesanta Software Limited
 * All rights reserved
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <inttypes.h>
#include <poll.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <openssl/ssl.h>

#define TEST_PORT 4343

static SSL_CTX *setup_ctx(const char *cert_chain, const char *cipher) {
  SSL_CTX *ctx;

  ctx = SSL_CTX_new(SSLv23_client_method());
  if (NULL == ctx) goto out;

  SSL_CTX_set_mode(ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

  /* always succeeds*/
  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                     NULL);

  if (!SSL_CTX_load_verify_locations(ctx, cert_chain, NULL)) goto out_free;

#ifdef OPENSSL_VERSION_NUMBER
  if (cipher != NULL) {
    SSL_CTX_set_cipher_list(ctx, cipher);
    fprintf(stderr, "Cipher spec: %s\n", cipher);
  }
#endif
  goto out;

out_free:
  SSL_CTX_free(ctx);
  ctx = NULL;
out:
  return ctx;
}

static int waitforit(SSL *ssl) {
  struct pollfd pfd;
  int ret;

  pfd.fd = SSL_get_fd(ssl);
  pfd.revents = 0;

  switch (SSL_get_error(ssl, -1)) {
    case SSL_ERROR_WANT_READ:
      pfd.events = POLLIN;
      break;
    case SSL_ERROR_WANT_WRITE:
      pfd.events = POLLOUT;
      break;
    default:
      return 0;
  }

  ret = poll(&pfd, 1, -1);
  if (ret != 1 || !(pfd.revents & pfd.events)) return 0;

  return 1;
}

static int do_connect(SSL *ssl) {
  int ret;

again:
  ret = SSL_connect(ssl);
  if (ret < 0) {
    if (waitforit(ssl)) {
      goto again;
    } else {
      return -1;
    }
  }

  return ret;
}

static int do_read(SSL *ssl, void *buf, int len) {
  int ret;

again:
  ret = SSL_read(ssl, buf, len);
  if (ret < 0) {
    if (waitforit(ssl)) {
      goto again;
    } else {
      return -1;
    }
  }

  return ret;
}

static int do_write(SSL *ssl, const void *buf, int len) {
  int ret;

again:
  ret = SSL_write(ssl, buf, len);
  if (ret < 0) {
    if (waitforit(ssl)) {
      goto again;
    } else {
      return -1;
    }
  }

  return ret;
}

static int do_shutdown(SSL *ssl) {
  int ret;

again:
  ret = SSL_shutdown(ssl);
  if (ret < 0) {
    if (waitforit(ssl)) {
      goto again;
    } else {
      return -1;
    }
  }

  return ret;
}

static int test_content(SSL *ssl) {
  static const char *const str1 = "Hello TLS1.2 world!";
  static const char *const str2 = "Hi yourself!";
  char buf[512];
  int ret;

  ret = do_write(ssl, str1, strlen(str1));
  if (ret < 0 || (size_t) ret != strlen(str1)) return 0;

  ret = do_read(ssl, buf, sizeof(buf));
  if (ret < 0 || (size_t) ret != strlen(str2)) return 0;
  printf("Got: %.*s\n", ret, buf);

  return !memcmp(buf, str2, ret);
}

static void ns_set_non_blocking_mode(int sock) {
#ifdef _WIN32
  unsigned long on = 1;
  ioctlsocket(sock, FIONBIO, &on);
#else
  int flags = fcntl(sock, F_GETFL, 0);
  fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
}

static int do_test(const char *cert_chain, const char *cipher) {
  struct sockaddr_in sa;
  struct pollfd pfd;
  SSL_CTX *ctx;
  SSL *ssl;
  int ret = 0;
  int fd;

  ctx = setup_ctx(cert_chain, cipher);
  if (NULL == ctx) goto out;

  ssl = SSL_new(ctx);
  if (NULL == ssl) goto out_ctx;

  fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) {
    fprintf(stderr, "socket: %s\n", strerror(errno));
    goto out_ssl;
  }
  ns_set_non_blocking_mode(fd);

  if (!SSL_set_fd(ssl, fd)) goto out_close;

  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  sa.sin_port = htons(TEST_PORT);
  if (connect(fd, (struct sockaddr *) &sa, sizeof(sa))) {
    if (errno != EINPROGRESS) {
      fprintf(stderr, "connect: %s\n", strerror(errno));
      goto out_close;
    }
  }

  pfd.events = POLLOUT;
  pfd.fd = fd;
  poll(&pfd, 1, -1);

  if (do_connect(ssl) <= 0) {
    printf("TLS connect failed\n");
    goto shutdown;
  }

  if (!test_content(ssl)) {
    printf("TLS data xfer failed\n");
    goto shutdown;
  }

  ret = 1;

shutdown:
  if (do_shutdown(ssl) > 0 && ret) {
    printf("SUCCESS\n");
  } else {
    ret = 0;
  }
out_close:
  close(fd);
out_ssl:
  SSL_free(ssl);
out_ctx:
  SSL_CTX_free(ctx);
out:
  return ret;
}

int main(int argc, char **argv) {
  const char *cipher = argc > 1 ? argv[1] : NULL;
  SSL_library_init();
  if (!do_test("ca.crt", cipher)) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
