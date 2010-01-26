/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * See LICENSE.txt included in this distribution for the specific
 * language governing permissions and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at LICENSE.txt.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Portions Copyright 2009 Matt Ingenthron
 */

#include "config.h"

#include "libmemc.h"

#if HAVE_PROTOCOL_BINARY
#include <memcached/protocol_binary.h>
#endif
#include <sys/types.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#ifndef __WIN32__
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <sys/uio.h>
#else
#include "win32/win32.h"
#endif

struct Server {
   int sock;
   struct addrinfo *addrinfo;
   const char *errmsg;
   const char *peername;
   char *buffer;
   int buffersize;
};

enum StoreCommand {add, set, replace};

struct Memcache {
   struct Server** servers;
   enum Protocol protocol;
   int no_servers;
};

static struct Server* server_create(const char *name, in_port_t port);
static void server_destroy(struct Server *server);
static int textual_store(struct Server* server, enum StoreCommand cmd, 
                        const struct Item *item);
static int textual_get(struct Server* server, struct Item* item);
static int binary_store(struct Server* server, enum StoreCommand cmd, 
                        const struct Item *item);
static int binary_get(struct Server* server, struct Item* item);
static int libmemc_store(struct Memcache* handle, enum StoreCommand cmd, const struct Item *item);
static struct Server *get_server(struct Memcache *handle, const char *key);
static int server_connect(struct Server *server);


/**
 * External interface
 */
struct Memcache* libmemc_create(enum Protocol protocol) {
   struct Memcache* ret = calloc(1, sizeof(struct Memcache));
   if (ret != NULL) {
      ret->protocol = protocol;
   }
   return ret;
}

void libmemc_destroy(struct Memcache* handle) {
   for (int ii = 0; ii < handle->no_servers; ++ii) {
      server_destroy(handle->servers[ii]);
   }
   free(handle);
}

int libmemc_add_server(struct Memcache *handle, const char *host, in_port_t port) {
   struct Server** servers = calloc(handle->no_servers + 1, sizeof(struct Server));
   struct Server** old = handle->servers;
    
   if (servers == 0) {
      return -1;
   }
    
   for (int ii = 0; ii < handle->no_servers; ++ii) {
      servers[ii] = handle->servers[ii];
   }
    
   handle->servers = servers;
   free(old);
    
   struct Server *server = server_create(host, port);
   if (server != NULL) {
      handle->servers[handle->no_servers++] = server;
   }
    
   return 0;
}

int libmemc_add(struct Memcache *handle, const struct Item *item) {
   return libmemc_store(handle, add, item);
}

int libmemc_set(struct Memcache *handle, const struct Item *item) {
   return libmemc_store(handle, set, item);    
}

int libmemc_replace(struct Memcache *handle, const struct Item *item) {
   return libmemc_store(handle, replace, item);        
}

int libmemc_get(struct Memcache *handle, struct Item *item) {
   struct Server* server = get_server(handle, item->key);
   if (server == NULL) {
      return -1;
   } else {
      if (server->sock == -1) {
         if (server_connect(server) == -1) {
            fprintf(stderr, "%s\n", server->errmsg);
            fflush(stderr);
            return -1;
         }
      }

      if (handle->protocol == Binary) {
         return binary_get(server, item);
      } else {
         return textual_get(server, item);
      }
   }
}

static struct addrinfo *lookuphost(const char *hostname, in_port_t port)
{
    struct addrinfo *ai = 0;
    struct addrinfo hints = {0};
    char service[NI_MAXSERV];
    int error;

    hints.ai_flags = AI_PASSIVE|AI_ADDRCONFIG;
    hints.ai_family = AF_UNSPEC;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_socktype = SOCK_STREAM;
    
    (void)snprintf(service, NI_MAXSERV, "%d", port);
    if ((error = getaddrinfo(hostname, service, &hints, &ai)) != 0) {
       if (error != EAI_SYSTEM) {
          fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(error));
       } else {
          perror("getaddrinfo()");
       }
    }

    return ai;
}

int libmemc_connect_server(const char *hostname, in_port_t port)
{
    struct addrinfo *ai = lookuphost(hostname, port);
    int sock = -1;
    if (ai != NULL) { 
       if ((sock = socket(ai->ai_family, ai->ai_socktype,
                          ai->ai_protocol)) != -1) {
          if (connect(sock, ai->ai_addr, ai->ai_addrlen) == -1) {
             fprintf(stderr, "Failed to connect socket: %s\n",
                     strerror(errno));
             close(sock);
             sock = -1;
          }
       } else {
          fprintf(stderr, "Failed to create socket: %s\n", strerror(errno));
       }
    
       freeaddrinfo(ai);
    }
    return sock;
}

/**
 * Internal functions used by both protocols
 */
static uint32_t simplehash(const char *key) {
   if (key == 0) {
      return 0;
   }
   uint32_t ret = 0;
   for (ret = *key; *key != 0; ++key) {
      ret = (ret << 4) + *key;
   }
   return ret;
}

static struct Server *get_server(struct Memcache *handle, const char *key) {
   if (handle->no_servers == 1) {
      return handle->servers[0];
   } else if (handle->no_servers > 0) {
      int idx = simplehash(key) % handle->no_servers;
      return handle->servers[idx];
   } else {
      return NULL;
   }
}

static int libmemc_store(struct Memcache* handle, enum StoreCommand cmd, 
                         const struct Item *item) {
   struct Server* server = get_server(handle, item->key);
   if (server == NULL) {
      return -1;
   } else {
      if (server->sock == -1) {
         if (server_connect(server) == -1) {
            return -1;
         }
      }
      
      if (handle->protocol == Binary) {
         return binary_store(server, cmd, item);
      } else {
         return textual_store(server, cmd, item);
      }
   }
}

static size_t server_receive(struct Server* server, char* data, size_t size, int line);
static int server_sendv(struct Server* server, struct iovec *iov, int iovcnt);
static int server_send(struct Server* server, const void *data, size_t size);
static int server_connect(struct Server *server);
static void server_disconnect(struct Server *server);

void server_destroy(struct Server *server) {
   if (server != NULL) {
      if (server->sock != -1) {
         close(server->sock);
      }
      free(server->buffer);
      free(server);
   }
}

struct Server* server_create(const char *name, in_port_t port) {
   struct addrinfo* ai = lookuphost(name, port);
   struct Server* ret = NULL;
   if (ai != NULL) {
      ret = calloc(1, sizeof(struct Server));
      if (ret != 0) {
         char buffer[1024];         
         ret->sock = -1;
         ret->errmsg = 0;
         ret->addrinfo = ai;
         sprintf(buffer, "%s:%d", name, port);
         ret->peername = strdup(buffer);
         ret->buffer = malloc(65 * 1024);
         ret->buffersize = 65 * 1024;
         server_connect(ret);
         if (ret->buffer == NULL) {
            server_destroy(ret);
            ret = 0;
         }
      }
   }
    
   return ret;
}

static void server_disconnect(struct Server *server) {
   if (server->sock != -1) {
      (void)close(server->sock);
      server->sock = -1;
   }
}

static int server_connect(struct Server *server)
{
   int flag = 1;
   
   if ((server->sock = socket(server->addrinfo->ai_family,
                              server->addrinfo->ai_socktype,
                              server->addrinfo->ai_protocol)) == -1) {
      char errmsg[1024];
      sprintf(errmsg, "Failed to create socket: %s", strerror(errno));
      server->errmsg = strdup(errmsg);
      return -1;
   }

   if (setsockopt(server->sock, IPPROTO_TCP, TCP_NODELAY,
                  &flag, sizeof(flag)) == -1) {
      perror("Failed to set TCP_NODELAY");
   }
   
   if (connect(server->sock, server->addrinfo->ai_addr,
               server->addrinfo->ai_addrlen) == -1) {
      char errmsg[1024];
      sprintf(errmsg, "Failed to connect socket: %s", strerror(errno));
      server->errmsg = strdup(errmsg);
      server_disconnect(server);
      return -1;
   }
    
   return 0;
}

static int server_send(struct Server* server, const void *data, size_t size) {
   size_t offset = 0;
   do {
      ssize_t sent = send(server->sock, ((const char*)data) + offset, size - offset, 0);
      if (sent == -1) {
         if (errno != EINTR) {
            char errmsg[1024];
            sprintf(errmsg, "Failed to send data to server: %s", strerror(errno));
            server->errmsg = strdup(errmsg);
            server_disconnect(server);
            return -1;
         }
      } else {
         offset += sent;
      }
   } while (offset < size);
    
   return 0;
}

static int server_sendv(struct Server* server, struct iovec *iov, int iovcnt) {
#ifdef WIN32
   // @todo I might have a scattered IO function on windows...
   for (int ii = 0; ii < iovcnt; ++ii) {
      if (send(server, iov[ii].iov_base, iov[ii].iov_len,0) != 0) {
         return -1;
      }
   }
#else
   // @todo Verify implementation if the writev returns with partitial
   // writes!
   size_t size = 0;
   for (int ii = 0;  ii < iovcnt; ++ ii) {
      size += iov[ii].iov_len;
   }
    
   do {
      ssize_t sent = writev(server->sock, iov, iovcnt);
      if (sent == -1) {
         if (errno != EINTR) {
            char errmsg[1024];
            sprintf(errmsg, "Failed to send data to server: %s", strerror(errno));
            server->errmsg = strdup(errmsg);
            server_disconnect(server);
            return -1;
         }
      } else {
         if (sent == size) {
            return 0;
         }
            
         for (int ii = 0; ii < iovcnt && sent > 0; ++ii) {
            if (iov[ii].iov_len < sent) {
               size -= iov[ii].iov_len;
               sent -= iov[ii].iov_len;
               iov[ii].iov_len = 0;
            } else {
#ifdef __sun
               iov[ii].iov_base += sent;
#else
               // iov_base is a void pointer...
               iov[ii].iov_base = ((char*)iov[ii].iov_base) + sent;
#endif
               iov[ii].iov_len -= sent;
               size -= sent;
               break;
            }
         }
      }
   } while (size > 0);
#endif
   return 0;
}

static size_t server_receive(struct Server* server, char* data, size_t size, int line) {
   size_t offset = 0;
   int stop = 0;
   do {
      ssize_t nread = recv(server->sock, data + offset, size - offset, 0);
      if (nread == -1) {
         if (errno != EINTR) {
            char errmsg[1024];
            sprintf(errmsg, "Failed to receive data from server: %s", strerror(errno));
            server->errmsg = strdup(errmsg);
            server_disconnect(server);
            return -1;
         }
      } else {
         if (line) {
            if (strchr(data + offset, '\r') != 0) {
               stop = 1;
            }
         }
         offset += nread;
      }
   } while (offset < size && !stop);
    
   if (line && !stop) {
      server->errmsg = strdup("Protocol error");
      server_disconnect(server);
      return -1;
   }
    
   return offset;
}


/* Byte swap a 64-bit number */
static int64_t swap64(int64_t in) {
#ifndef __sparc
    /* Little endian, flip the bytes around until someone makes a faster/better
    * way to do this. */
    int64_t rv = 0;
    int i = 0;
     for(i = 0; i < 8; i++) {
        rv = (rv << 8) | (in & 0xff);
        in >>= 8;
     }
    return rv;
#else
    /* big-endian machines don't need byte swapping */
    return in;
#endif
}


/**
 * Implementation of the Binary protocol
 */
static int binary_get(struct Server* server, struct Item* item) 
{
#if HAVE_PROTOCOL_BINARY
   uint16_t keylen = item->keylen;
   uint32_t bodylen = keylen;

   protocol_binary_request_get request = { .bytes = {0} };
   request.message.header.request.magic = PROTOCOL_BINARY_REQ;
   request.message.header.request.opcode = PROTOCOL_BINARY_CMD_GET;
   request.message.header.request.keylen = htons(keylen);
   request.message.header.request.datatype = PROTOCOL_BINARY_RAW_BYTES;
   request.message.header.request.bodylen = htonl(bodylen);
   
   struct iovec iovec[2];
   iovec[0].iov_base = (void*)&request;
   iovec[0].iov_len = sizeof(request);
   iovec[1].iov_base = (void*)item->key;
   iovec[1].iov_len = keylen;

   server_sendv(server, iovec, 2);

   protocol_binary_response_set response;
   size_t nread = server_receive(server, (char*)response.bytes,
                                 sizeof(response.bytes), 0);
   if (nread != sizeof(response)) {
      server->errmsg = strdup("Protocol error");
      server_disconnect(server);      
      return -1;
   }

   bodylen = ntohl(response.message.header.response.bodylen);
   if (response.message.header.response.status == 0) {
      if (item->data != NULL) {
         if ((bodylen-response.message.header.response.extlen) > item->size) {
            free(item->data);
            item->data = NULL;
         }
      }

      if (item->data == NULL) {
         item->size = bodylen - response.message.header.response.extlen;
         item->data = malloc(item->size);
         if (item->data == NULL) {
            server->errmsg = strdup("failed to allocate memory\n");
            server_disconnect(server);      
            return -1;
         }
      }
      
      if (response.message.header.response.extlen != 0) {
         assert(response.message.header.response.extlen == 4);
         uint32_t flags;
         struct iovec iovec[2];
         iovec[0].iov_base = (void*)&flags;
         iovec[0].iov_len = sizeof(flags);
         iovec[1].iov_base = item->data;
         iovec[1].iov_len = item->size;

#ifndef __WIN32__
         ssize_t nread = readv(server->sock, iovec, 2);
#else
         ssize_t nread = WSARecv(server->sock, iovec, 2, NULL, NULL, NULL, NULL);
#endif
         if (nread < bodylen) {
             // partial read.. read the rest!
             nread -= 4;
             size_t left = item->size - nread;
             if (server_receive(server, item->data + nread, left, 0) != left) {
                 abort();
             }
         }
      } else {
         size_t nread = server_receive(server, item->data, item->size, 0);
         assert(nread == item->size);
      }
      
      item->cas_id = swap64(response.message.header.response.cas);
   } else {
      char *buffer = malloc(bodylen + 1);
      if (buffer == NULL) {
         server->errmsg = strdup("failed to allocate memory\n");
         server_disconnect(server);      
         return -1;
      }
      buffer[bodylen] = '\0';
      server_receive(server, buffer, bodylen, 0);
      server->errmsg = buffer;
      
      return -1;
   }

   return 0;
#endif
   return -1;
}

static int binary_store(struct Server* server, 
                         enum StoreCommand cmd, 
                         const struct Item *item)  
{
#if HAVE_PROTOCOL_BINARY
   protocol_binary_request_set request = { .bytes = {0} };
   request.message.header.request.magic = PROTOCOL_BINARY_REQ;

   switch (cmd) {
   case add :
      request.message.header.request.opcode = PROTOCOL_BINARY_CMD_ADD; break;
   case set :
      request.message.header.request.opcode = PROTOCOL_BINARY_CMD_SET; break;
   case replace :
      request.message.header.request.opcode = PROTOCOL_BINARY_CMD_REPLACE; break;
   default:
      abort();
   }

   uint16_t keylen = item->keylen;
   request.message.header.request.keylen = htons(keylen);
   request.message.header.request.extlen = 8;
   request.message.header.request.datatype = 0;
   request.message.header.request.reserved = 0;
   request.message.header.request.bodylen = htonl(keylen + item->size + 8);
   request.message.header.request.opaque = 0;
   request.message.header.request.cas = swap64(item->cas_id);
   request.message.body.flags = 0;
   request.message.body.expiration = htonl(item->exptime);
   
   struct iovec iovec[3];
   iovec[0].iov_base = (void*)&request;
   iovec[0].iov_len = sizeof(request);
   iovec[1].iov_base = (void*)item->key;
   iovec[1].iov_len = keylen;
   iovec[2].iov_base = item->data;
   iovec[2].iov_len = item->size;

   server_sendv(server, iovec, 3);

   protocol_binary_response_set response;
   size_t nread = server_receive(server, (char*)response.bytes,
                                 sizeof(response.bytes), 0);
   if (nread != sizeof(response)) {
      server->errmsg = strdup("Protocol error");
      server_disconnect(server);      
      return -1;
   }

   if (response.message.header.response.status == 0 &&
       response.message.header.response.bodylen != 0) {
      server->errmsg = strdup("Unexpected data returned\n");
      server_disconnect(server);      
      return -1;
   } else if (response.message.header.response.bodylen != 0) {
      uint32_t len = ntohl(response.message.header.response.bodylen);
      char* buffer = malloc(len);
      if (buffer == 0) {
         server->errmsg = strdup("failed to allocate memory\n");
         server_disconnect(server);      
         return -1;
      }

      size_t nread = server_receive(server, buffer, len, 0);
      free(buffer);
   }

   return (response.message.header.response.status == 0) ? 0 : -1;
#endif
  return -1;
}

/**
 * Implementation of the Textual protocol
 */
static int parse_value_line(char *header, uint32_t* flag, size_t* size, char** data) {
   char *end = strchr(header, ' ');
   if (end == 0) {
      return -1;
   }
   char *start = end + 1;
   *flag = (uint32_t)strtoul(start, &end, 10);
   if (start == end) {
      return -1;
   }
   start = end + 1;
   *size = (size_t)strtoul(start, &end, 10);
   if (start == end) {
      return -1;
   }
   if (strstr(end, "\r\n") != end) {
      return -1;
   }
    
   *data = end + 2;
   return 0;
}

static int textual_get(struct Server* server, struct Item* item) {    
   uint32_t flag;
   
   struct iovec iovec[3];
   iovec[0].iov_base = (char*)"get ";
   iovec[0].iov_len = 4;
   iovec[1].iov_base = (char*)item->key;
   iovec[1].iov_len = item->keylen;
   iovec[2].iov_base = (char*)"\r\n";
   iovec[2].iov_len = 2;
   server_sendv(server, iovec, 3);
   
   size_t nread = server_receive(server, server->buffer,server->buffersize, 1);
    
   // Split the header line
   if (strstr(server->buffer, "VALUE ") == server->buffer) {
      size_t elemsize;
      char *ptr;
        
      if (parse_value_line(server->buffer + 6, &flag, &elemsize, &ptr) == -1){
         server->errmsg = strdup("Protocol error");
         server_disconnect(server);
         return -1;
      }
#ifndef __sun
      typedef size_t ptrdiff_t;
#endif
      ptrdiff_t headsize = ptr - server->buffer;
      size_t chunk = nread - headsize;
        
      if (chunk < (elemsize + 7)) {
         // I don't have all of the data.. keep on reading
         server_receive(server, server->buffer + nread,
                        (elemsize - chunk) + 7, 0);
      }
        
      void *result = ptr;        
      if (elemsize > item->size) {
         if (item->data != 0) {
            free(item->data);
         }
         item->size = elemsize;
         item->data = malloc(item->size);
         if (item->data == 0) {
            item->size = 0;
            return -1;
         }
      }
        
      memcpy(item->data, result, item->size);
      return 0;
   } else if (strstr(server->buffer, "END") == server->buffer) {
      return -1;
   }

   abort();
}

static int textual_store(struct Server* server, 
                         enum StoreCommand cmd, 
                         const struct Item *item)  {
   static const char* const commands[] = { "add ", "set ", "replace " };
    
   uint32_t flags = 0;
   const void *dta = item->data;
   size_t size = item->size;    
   ssize_t len = sprintf(server->buffer, " %d %ld %ld\r\n", 
           flags, (long)item->exptime, (long)item->size);
    
   struct iovec iovec[5];
   iovec[0].iov_base = (char*)commands[cmd];
   iovec[0].iov_len = strlen(commands[cmd]);
   iovec[1].iov_base = (char*)item->key;
   iovec[1].iov_len = item->keylen;
   iovec[2].iov_base = server->buffer;
   iovec[2].iov_len = len;
   iovec[3].iov_base = (char*)dta;
   iovec[3].iov_len = size;
   iovec[4].iov_base = (char*)"\r\n";
   iovec[4].iov_len = 2;
   server_sendv(server, iovec, 5);
    
   size_t offset = 0;
   do {
      len = recv(server->sock, (void*)(server->buffer + offset),
                 server->buffersize - offset, 0);
      if (len == -1) {
         if (errno != EINTR) {
            char errmsg[1024];
            sprintf(errmsg, "Failed to receive data from server: %s",
                    strerror(errno));
            server->errmsg = strdup(errmsg);
            server_disconnect(server);
            return -1;
         }
      } else if (len == 0) {
         server->errmsg = strdup("Lost contact with server");
         server_disconnect(server);
         return -1;
      } else {
         offset += len;
         if (strchr(server->buffer, '\r') != 0) {
            if (strstr(server->buffer, "STORED\r\n") == server->buffer) {
               return 0;
            } else if (strstr(server->buffer,
                              "NOT_STORED\r\n") == server->buffer) {
               server->errmsg = strdup("Item NOT stored");
               return -1;
            }
         }
      }
      if (offset == server->buffersize) {
         server->errmsg = strdup("Out of sync with server...");
         server_disconnect(server);
         return -1;
      }
   } while (1);
}
