// Web socket library
// (c) 2017 Adrian Kennard Andrews & Arnold ltd
//
// Library for web sockets server.
//
// The concept is that this allows an application to bind to accept web socket connections
// The library manages the connections and threads for sending and receiving data
// The library is designed to send and receive json messages, these are coded using axl
// 
// The command line is purely for testing. It is expected a generic push server will be
// added in due course.
//

    /*
       Copyright (C) 2017  RevK and Andrews & Arnold Ltd

       This program is free software: you can redistribute it and/or modify
       it under the terms of the GNU General Public License as published by
       the Free Software Foundation, either version 3 of the License, or
       (at your option) any later version.

       This program is distributed in the hope that it will be useful,
       but WITHOUT ANY WARRANTY; without even the implied warranty of
       MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
       GNU General Public License for more details.

       You should have received a copy of the GNU General Public License
       along with this program.  If not, see <http://www.gnu.org/licenses/>.
     */

#include <stdio.h>
#include <string.h>
#include <popt.h>
#include <time.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ctype.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <err.h>
#include <execinfo.h>
#include <pthread.h>
#include <axl.h>
#include <websocket.h>

#ifndef	MAXTCP
#define MAXTCP 32768            // we use a lot of sockets but usually short messages, so reduce footprint
#endif

const char wscookie[] = "wssession";

int websocket_debug = 0;

typedef struct websocket_bind_s websocket_bind_t;
typedef struct websocket_path_s websocket_path_t;
typedef websocket_t *websocket_p;

typedef struct txb_s txb_t;
struct txb_s
{
   volatile int count;          // how many instances in txqs
   pthread_mutex_t mutex;       // protect count
   unsigned char head[14];
   unsigned char hlen;
   unsigned char *buf;
   size_t len;
};

typedef struct txq_s txq_t;
typedef txq_t *txq_p;
struct txq_s
{                               // Queue of transmit data
   volatile txq_p next;
   txb_t *data;
};

struct websocket_bind_s
{                               // The bound ports / threads
   websocket_bind_t *next;
   const char *port;
   const char *certfile;
   const char *keyfile;         // NULL if not ssl
   SSL_CTX *ctx;                // SSL context
   int socket;                  // listening socket
   websocket_path_t *paths;
   pthread_mutex_t mutex;       // Protect sessions
   volatile websocket_p sessions;
};

struct websocket_path_s
{                               // The bound paths on a port
   websocket_path_t *next;
   const char *host;            // Check host (null=wildcard)
   const char *path;            // Check path (null=wildcard)
   const char *origin;          // Check origin (null=wildcard)
   websocket_callback_t *callback;
   websocket_callback_raw_t *callbackraw;
};

struct websocket_s
{                               // The specific web socket instance
   volatile websocket_p next;
   websocket_bind_t *bind;
   websocket_path_t *path;
   char *from;
   SSL *ss;                     // SSL connection if applicable, else NULL
   unsigned char *rxdata;       // Received data so far (malloc)
   size_t rxptr;                // Pointer in to buffer
   size_t rxlen;                // Length of buffer allocated
   void *data;                  // App data link
   long ping;                   // Ping time (us)
   pthread_mutex_t mutex;       // Protect volatile
   volatile txq_p txq,
     txe;
   volatile int socket;         // rx socket
   volatile int pipe[2];        // pipe used to kick tx
   volatile unsigned char connected:1;
   volatile unsigned char closed:1;
};

unsigned long
websocket_ping (websocket_t * w)
{                               // Ping data
   if (!w)
      return 0;
   return w->ping;
}

void *
websocket_data (websocket_t * w)
{                               // Link to a void*
   if (!w)
      return NULL;
   return w->data;
}

void
websocket_set_data (websocket_t * w, void *data)
{                               // Link to a void*
   if (!w)
      return;
   w->data = data;
}

static websocket_bind_t *binds = NULL;
static void
txb_done (txb_t * b)
{                               // Count down and maybe even free
   pthread_mutex_lock (&b->mutex);
   int c = --b->count;
   pthread_mutex_unlock (&b->mutex);
   if (!c)
   {                            // free
      free (b->buf);
      free (b);
   }
}

static txb_t *
txb_new_data (size_t len, const unsigned char *buf)
{                               // Make a block from XML (count set to 1) - assuming buf malloc'd
   txb_t *txb = malloc (sizeof (*txb));
   memset (txb, 0, sizeof (*txb));
   pthread_mutex_init (&txb->mutex, NULL);
   txb->len = len;
   txb->buf = (unsigned char *) buf;
   txb->count = 1;              // Initial count to one so not zapped whilst adding to queues
   int p = 0;
   if (!buf)
   {                            // close
      txb->head[0] = 0x88;      // close
      txb->head[1] = 0;         // zero len
      txb->hlen = 2;
   } else
   {
      txb->head[p++] = 0x81;    // Text, one block
      if (len > 65535)
      {
         txb->head[p++] = 127;
         txb->head[p++] = ((unsigned long long) len >> 56);
         txb->head[p++] = ((unsigned long long) len >> 48);
         txb->head[p++] = ((unsigned long long) len >> 40);
         txb->head[p++] = ((unsigned long long) len >> 32);
         txb->head[p++] = (len >> 24);
         txb->head[p++] = (len >> 16);
         txb->head[p++] = (len >> 8);
         txb->head[p++] = (len);
      } else if (len >= 126)
      {
         txb->head[p++] = 126;
         txb->head[p++] = (len >> 8);
         txb->head[p++] = (len);
      } else
         txb->head[p++] = len;
      txb->hlen = p;
   }
   return txb;
}

static txb_t *
txb_new (xml_t d)
{                               // Make a block from XML (count set to 1)
   if (!d)
      return txb_new_data (0, NULL);
   char *buf = NULL;
   size_t len = 0;
   FILE *out = open_memstream (&buf, &len);
   if (d)
      xml_write_json (out, d);
   fclose (out);
   return txb_new_data (len, (unsigned char *) buf);
}

static void
txb_queue (websocket_t * w, txb_t * txb)
{                               // Add a block to a websocket (NULL means close)
   txq_t *txq = malloc (sizeof (*txq));
   memset (txq, 0, sizeof (*txq));
   txq->data = txb;
   pthread_mutex_lock (&txb->mutex);
   txb->count++;
   pthread_mutex_unlock (&txb->mutex);
   pthread_mutex_lock (&w->mutex);
   if (w->txq)
      w->txe->next = txq;
   else
      w->txq = txq;
   w->txe = txq;
   pthread_mutex_unlock (&w->mutex);
   char poke = 0;
   pthread_mutex_lock (&w->mutex);
   if (w->pipe[1] >= 0)
      write (w->pipe[1], &poke, sizeof (poke));
   pthread_mutex_unlock (&w->mutex);
}

void *
websocket_tx (void *p)
{                               // Tx thread
   sigignore (SIGPIPE);
   websocket_t *w = p;
   void nextq (void)
   {                            // Unlink queue
      pthread_mutex_lock (&w->mutex);
      txq_t *q = (txq_t *) w->txq;
      w->txq = q->next;
      pthread_mutex_unlock (&w->mutex);
      txb_done (q->data);
      free (q);                 // queue freed
   }
   time_t nextping = time (0) + 2;
   while (1)
   {
      time_t now = time (0);
      // Send data if we can
      if (w->txq && w->connected)
      {
         ssize_t len = 0;
         size_t ptr = 0;
         if (w->txq->data->hlen)
         {                      // Header
            if ((w->txq->data->head[0] & 0x0F) == 0x08)
               w->closed = 1;   // Sent a close
            while (ptr < w->txq->data->hlen)
            {
               if (w->ss)
                  len = SSL_write (w->ss, w->txq->data->head + ptr, w->txq->data->hlen - ptr);
               else
                  len = send (w->socket, w->txq->data->head + ptr, w->txq->data->hlen - ptr, 0);
               if (len <= 0)
                  break;
               ptr += len;
            }
            if (websocket_debug)
            {
               fprintf (stderr, "Tx Header");
               int p;
               for (p = 0; p < w->txq->data->hlen; p++)
                  fprintf (stderr, " %02X", w->txq->data->head[p]);
               fprintf (stderr, "\n");
            }
         }
         ptr = 0;
         while (ptr < w->txq->data->len)
         {
            if (w->ss)
               len = SSL_write (w->ss, w->txq->data->buf + ptr, w->txq->data->len - ptr);
            else
               len = send (w->socket, w->txq->data->buf + ptr, w->txq->data->len - ptr, 0);
            if (len <= 0)
               break;           // Failed
            if (websocket_debug)
               fprintf (stderr, "Tx [%.*s]\n", (int) len, w->txq->data->buf + ptr);
            ptr += len;
         }
         nextq ();
         if (w->closed || len <= 0)
            break;
         if (w->txq)
            continue;           // More data
      } else if (now > nextping)
      {                         // Send Ping
         nextping = now + 60;
         struct timeval tv;
         struct timezone tz;
         gettimeofday (&tv, &tz);
         unsigned long long us = tv.tv_sec * 1000000ULL + tv.tv_usec;;
         unsigned char ping[2 + sizeof (us)] = { 0x89, sizeof (us) };
         memcpy (ping + 2, &us, sizeof (us));
         int p = 0,
            l = sizeof (ping),
            len;
         if (websocket_debug)
         {
            fprintf (stderr, "Tx ");
            for (len = 0; len < l; len++)
               fprintf (stderr, " %02X", ping[len]);
            fprintf (stderr, "\n");
         }
         while (p < l)
         {
            if (w->ss)
               len = SSL_write (w->ss, ping + p, l - p);
            else
               len = send (w->socket, ping + p, l - p, 0);
            if (len <= 0)
               break;
            p += len;
         }
      }
      struct pollfd p = { w->pipe[0], POLLIN, 0 };
      int s = poll (&p, 1, (now < nextping) ? (nextping - now) * 1000 : 1000);
      if (!s)
         continue;
      if (s < 0)
         break;
      // Wait for new data to be added to queue
      char poke;
      ssize_t len = read (w->pipe[0], &poke, sizeof (poke));
      if (len <= 0)
         break;                 // Done
   }
   // Closed our pipe, so closed connection...
   if (websocket_debug)
      fprintf (stderr, "Closed connection from %s\n", w->from);
   while (w->txq)
      nextq ();                 // free
   if (w->connected && !w->closed)
   {                            // close
      char end[2] = { 0x88, 0x00 };
      if (w->ss)
         SSL_write (w->ss, end, 2);
      else
         send (w->socket, end, 2, 0);
   }
   if (w->ss)
      SSL_shutdown (w->ss);
   close (w->socket);
   {
      // This is to make sure the pipe is closed and not just outgoing broken
      char poke;
      while (read (w->pipe[0], &poke, sizeof (poke)) > 0);
   }
   pthread_mutex_lock (&w->mutex);
   if (w->ss)
   {
      SSL_free (w->ss);
      w->ss = NULL;
   }
   w->socket = -1;
   close (w->pipe[0]);
   w->pipe[0] = -1;
   pthread_mutex_unlock (&w->mutex);
   if (w->path && w->path->callbackraw && w->connected)
   {
      if (websocket_debug)
         fprintf (stderr, "%p Close callback\n", w);
      w->path->callbackraw (w, NULL, 0, NULL);  // Closed (we do not consider returned error)
   } else if (w->path && w->path->callback && w->connected)
   {
      if (websocket_debug)
         fprintf (stderr, "%p Close callback\n", w);
      w->path->callback (w, NULL, NULL);        // Closed (we do not consider returned error)
   }
   free (w->from);
   // Free web socket
   pthread_mutex_lock (&w->bind->mutex);
   websocket_t **ww;
   for (ww = (websocket_t **) & w->bind->sessions; *ww && *ww != w; ww = (websocket_t **) & (*ww)->next);
   *ww = (websocket_t *) w->next;
   pthread_mutex_unlock (&w->bind->mutex);
   free (w);
   pthread_exit (NULL);
   return NULL;
}

char *
websocket_do_rx (websocket_t * w)
{                               // Rx thread
   if (w->bind->keyfile)
   {                            // SSL set up
      w->ss = SSL_new (w->bind->ctx);
      if (!w->ss)
         return "Cannot create SSL server structure";
      if (!SSL_set_fd (w->ss, w->socket))
         return "Could not set client SSL fd";
      int r = SSL_accept (w->ss);
      if (r != 1)
         return "Could not establish SSL client connection";
   }
   {                            // Rx initial handshake
      unsigned int ep = 0;
      while (1)
      {
         struct pollfd p = { w->socket, POLLIN, 0 };
         int s = poll (&p, 1, 10000);
         if (s <= 0)
         {
            if (websocket_debug)
               fprintf (stderr, "Rx handshake [%.*s]\n", (int) w->rxptr, w->rxdata);
            return "Handshake timeout";
         }
         if (w->rxlen - w->rxptr < 1000)
            w->rxdata = realloc (w->rxdata, w->rxlen += 1000);
         ssize_t len = 0;
         if (w->ss)
            len = SSL_read (w->ss, w->rxdata + w->rxptr, w->rxlen - w->rxptr - 1);
         else
            len = recv (w->socket, w->rxdata + w->rxptr, w->rxlen - w->rxptr - 1, 0);
         if (len <= 0)
            return "Connection closed in handshake";
         w->rxptr += len;
         if (w->rxptr < 4)
            continue;
         while (ep <= w->rxptr - 4
                && (w->rxdata[ep + 0] != '\r' || w->rxdata[ep + 1] != '\n' || w->rxdata[ep + 2] != '\r'
                    || w->rxdata[ep + 3] != '\n'))
            ep++;
         if (ep <= w->rxptr - 4)
            break;
      }
      if (websocket_debug)
         fprintf (stderr, "Rx handshake [%.*s]\n", (int) ep, w->rxdata);
      // Process headers
      unsigned char *e = w->rxdata + ep + 2;
      ep += 4;
      // The command (GET/POST/etc)
      unsigned char *p;
      for (p = w->rxdata; p < e && isalpha (*p); p++)
         *p = tolower (*p);
      if (p >= e || *p != ' ')
         return "Bad request";
      while (p < e && *p == ' ')
         *p++ = 0;
      unsigned char *eol = p;
      while (eol < e && *eol > ' ')
         eol++;
      if (eol == p)
         return "Bad request";
      while (eol < e && *eol >= ' ')
         *eol++ = 0;
      if (eol < e && *eol == '\r')
         *eol++ = 0;
      if (eol < e && *eol == '\n')
         *eol++ = 0;
      xml_t head = xml_tree_new ((char *) w->rxdata);
      char *url = (char *) p;
      char *query = NULL;
      for (query = url; *query && *query != '?'; query++);
      if (*query == '?')
      {                         // decode query args and add to header too
         *query++ = 0;
         xml_t q = head;
         q = xml_add (head, "query", query);    // New format creates query as sub object of head
         while (*query)
         {                      // URL decode
            char *p = query,
               *v = NULL;
            while (*p && *p != '=' && *p != '&')
               p++;
            if (*p == '=')
            {
               *p++ = 0;
               v = p;
               char *o = p;
               while (*p && *p != '&')
               {
                  if (*p == '+')
                  {
                     *o++ = ' ';
                     p++;
                  } else if (*p == '%' && isxdigit (p[1]) && isxdigit (p[2]))
                  {
                     *o++ = (((p[1] & 0xF) + (isalpha (p[1]) ? 9 : 0)) << 4) + ((p[2] & 0xF) + (isalpha (p[2]) ? 9 : 0));
                     p += 3;
                  } else
                     *o++ = *p++;
               }
               if (o < p)
                  *o = 0;
            }
            if (*p == '&')
               *p++ = 0;
            xml_attribute_t a = xml_attribute_set (q, query, v ? : "null");
            if (a && !v)
               a->json_unquoted = 1;
            query = p;
         }
      }
      xml_t http = head;
      http = xml_element_add (head, "http");    // Sub object if using raw logic
      xml_element_set_content (http, (char *) p);       // The URL
      p = eol;                  // First header (these overwrite any user sent attributes)
      char *session = NULL;
      // Extract headers
      while (p < e)
      {
         unsigned char *eol = p;
         while (eol < e)
         {                      // End of line
            while (eol < e && *eol >= ' ')
               eol++;
            if (eol + 3 < e && eol[0] == '\r' && eol[1] == '\n' && (eol[2] == ' ' || eol[2] == '\t'))
            {
               eol += 2;
               continue;
            }
            break;
         }
         if (eol < e && *eol == '\r')
            *eol++ = 0;
         if (eol < e && *eol == '\n')
            *eol++ = 0;
         unsigned char *eoh = p;
         for (; eoh < e && (isalnum (*eoh) || *eoh == '-'); eoh++)
            *eoh = tolower (*eoh);
         while (p < eol && *eoh == ' ')
            *eoh++ = 0;
         if (*eoh == ':')
            *eoh++ = 0;
         while (eoh < eol && *eoh == ' ')
            *eoh++ = 0;
         if (!strcmp ((char *) p, "authorization") && !strncasecmp ((char *) eoh, "Basic ", 6))
         {
            eoh += 6;
            while (*eoh == ' ')
               eoh++;
            char *data = NULL;
            int l = xml_base64d ((char *) eoh, &data);
            if (data)
            {
               data = realloc (data, l + 1);
               data[l] = 0;
               xml_attribute_set (http, (char *) p, data);
               free (data);
            }
         } else
            xml_attribute_set (http, (char *) p, (char *) eoh);
         if (!strcmp ((char *) p, "cookie"))
         {                      // Scan for our session cookie
            char *data = (char *) eoh;
            int l = strlen (wscookie);
            while (*data)
            {
               while (*data && isspace (*data))
                  data++;
               if (!strncmp (data, wscookie, l) && !isalnum (data[l]))
               {                // Looks like our cookie
                  data += l;
                  while (*data && isspace (*data))
                     data++;
                  if (*data == '=')
                  {
                     data++;
                     while (*data && isspace (*data))
                        data++;
                     char *e = data;
                     while (*e && *e != ';')
                        e++;
                     while (e > data && isspace (e[-1]))
                        e--;
                     session = malloc (e + 1 - data);
                     if (!session)
                        errx (1, "malloc");
                     memcpy (session, data, e - data);
                     session[e - data] = 0;
                  }
                  break;
               }
               while (*data && *data != ';')
                  data++;
               if (*data == ';')
                  data++;
            }
         }
         p = eol;
         if (p == e || *p < ' ')
            break;              // Odd
      }
      if (!session)
      {                         // Make a session id
         session = malloc (65);
         if (!session)
            errx (1, "malloc");
         int r = open ("/dev/urandom", O_RDONLY);
         if (r < 0)
            err (1, "Random");
         if (read (r, session, 64) != 64)
            err (1, "Random");
         int p;
         for (p = 0; p < 64; p++)
            session[p] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"[session[p] & 31];
         session[p] = 0;
         close (r);
      }
      xml_add (head, "@session", session);
      char *host = xml_get (http, "@host");
      char *origin = xml_get (http, "@origin");
      xml_attribute_set (head, "IP", w->from);
      int mismatch (const char *ref, const char *val)
      {
         if (!ref)
            return 0;           // OK
         if (!val)
            return 1;           // Bad
         return strcmp (ref, val);      // comp
      }
      websocket_path_t *path;
      for (path = w->bind->paths;
           path && (mismatch (path->origin, origin) || mismatch (path->host, host) || mismatch (path->path, url));
           path = path->next);
      if (!path)
      {
         if (head)
            xml_tree_delete (head);
         free (session);
         return "Path not found";
      }
      w->path = path;
      char *er = NULL;
      char *v = xml_get (http, "@upgrade");
      if (!v)
      {                         // HTTP
         char *cl = xml_get (http, "@content-length");
         char *expect = xml_get (http, "@expect");
         if (!strcmp (xml_element_name (head), "post") || expect || cl)
         {                      // data to receive
            if (ep < w->rxptr)
            {
               memmove (w->rxdata, w->rxdata + ep, w->rxptr - ep);
               w->rxptr -= ep;
            } else
            {
               free (w->rxdata);
               w->rxdata = NULL;
               w->rxptr = 0;
               w->rxlen = 0;
            }
            size_t max = 0;
            if (cl)
            {
               max = strtoull (cl, NULL, 10);
               w->rxdata = realloc (w->rxdata, w->rxlen = max + 1);
               if (!w->rxdata)
                  er = "Malloc fail";
            }
            if (expect && !strncmp (expect, "100", 3))
            {
               char *reply = "HTTP/1.1 100 Continue\r\n\r\n";
               if (w->ss)
                  SSL_write (w->ss, reply, strlen (reply));
               else
                  send (w->socket, reply, strlen (reply), 0);
            }
            if (!er)
            {
               while (1)
               {
                  if (max && w->rxptr == max)
                     break;
                  if (!max && w->rxlen - w->rxptr < 1000)
                     w->rxdata = realloc (w->rxdata, w->rxlen += 1000);
                  ssize_t len = 0;
                  if (w->ss)
                     len = SSL_read (w->ss, w->rxdata + w->rxptr, w->rxlen - w->rxptr - 1);
                  else
                     len = recv (w->socket, w->rxdata + w->rxptr, w->rxlen - w->rxptr - 1, 0);
                  if (!len)
                     break;     // End of input
                  if (len < 0)
                  {
                     er = "Connection closed in handshake";
                     break;
                  }
                  w->rxptr += len;
               }
               w->rxdata[w->rxptr] = 0;
               if (websocket_debug)
                  fprintf (stderr, "Parse [%s]\n", (char *) w->rxdata);
               if (w->path && w->path->callbackraw)
               {                // Raw data callback
                  if (websocket_debug)
                     fprintf (stderr, "%p Post callback\n", w);
                  er = w->path->callbackraw (NULL, head, w->rxptr, w->rxdata);
                  head = NULL;  // assumed to consume head/data
                  w->rxdata = NULL;     // consumed
                  w->rxptr = 0;
               } else if (w->path && w->path->callback)
               {                // Note can call a post with null if nothing posted
                  xml_t data = xml_tree_parse_json ((char *) w->rxdata, "json");
                  if (websocket_debug)
                     fprintf (stderr, "%p Post callback\n", w);
                  er = w->path->callback (NULL, head, data);
                  head = NULL;  // assumed to consume head/data
               }
            }
         } else if (w->path && w->path->callbackraw)
         {
            if (websocket_debug)
               fprintf (stderr, "%p Get callback\n", w);
            er = w->path->callbackraw (NULL, head, 0, NULL);
            head = NULL;
         } else if (w->path && w->path->callback)
         {
            if (websocket_debug)
               fprintf (stderr, "%p Get callback\n", w);
            er = w->path->callback (NULL, head, NULL);
            head = NULL;
         }
         if (!er)
            er = "204 No content";
      } else
      {                         // Web socket
         host = strdupa (host); // We free before using it otherwise
         {                      // Strip port
            char *p = strrchr (host, ':');
            if (p)
               *p = 0;
         }
         unsigned char hash[SHA_DIGEST_LENGTH] = { };
         if (strcmp (xml_element_name (head), "get"))
            er = "Bad request (not GET)";
         if (strcasecmp (v, "websocket"))
            er = "Bad upgrade header (not websocket)";
         v = xml_get (http, "@sec-websocket-version");
         if (!v)
            er = "No version";
         else if (atoi (v) != 13)
            er = "Bad version (not 13)";
         v = xml_get (http, "@sec-websocket-key");
         if (!v)
            er = "No websocket key";
         else
         {
            SHA_CTX c;
            SHA1_Init (&c);
            SHA1_Update (&c, v, strlen (v));
            SHA1_Update (&c, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 36);
            SHA1_Final (hash, &c);
         }
         if (!er && w->path->callbackraw)
         {
            if (websocket_debug)
               fprintf (stderr, "%p Connect callback\n", w);
            er = w->path->callbackraw (w, head, 0, NULL);
            head = NULL;
         } else if (!er && w->path->callback)
         {
            if (websocket_debug)
               fprintf (stderr, "%p Connect callback\n", w);
            er = w->path->callback (w, head, NULL);
            head = NULL;
         }
         if (!er)
         {                      // Response...
            txb_t *txb = malloc (sizeof (*txb));
            memset (txb, 0, sizeof (*txb));
            pthread_mutex_init (&txb->mutex, NULL);
            txb->count = 1;
            txb->len = asprintf ((char **) &txb->buf,   //
                                 "HTTP/1.1 101 Switching Protocols\r\n" //
                                 "Upgrade: websocket\r\n"       //
                                 "Connection: Upgrade\r\n"      //
                                 "Set-Cookie: %s=%s; Path=%s; Domain=%s%s\r\n"  //
                                 "Sec-WebSocket-Accept: %s\r\n" //
                                 "\r\n",        //
                                 wscookie, session, path->path ? : url, host, w->ss ? "; Secure" : "",
                                 xml_base64 (SHA_DIGEST_LENGTH, hash));
            if (txb->len <= 0)
               er = "Bad asprintf";
            else
            {                   // Switch protocols message to front of queue
               // Like txb_queue, but at start of queue
               txq_t *txq = malloc (sizeof (*txq));
               memset (txq, 0, sizeof (*txq));
               txq->data = txb;
               pthread_mutex_lock (&txb->mutex);
               txb->count++;
               pthread_mutex_unlock (&txb->mutex);
               pthread_mutex_lock (&w->mutex);
               if (w->txq)
                  txq->next = w->txq;
               else
                  w->txe = txq;
               w->txq = txq;
               w->connected = 1;        // Allows tx to start
               pthread_mutex_unlock (&w->mutex);
               char poke = 0;
               pthread_mutex_lock (&w->mutex);
               if (w->pipe[1] >= 0)
                  write (w->pipe[1], &poke, sizeof (poke));
               pthread_mutex_unlock (&w->mutex);
               txb_done (txb);
            }
         }
      }
      if (head)
         xml_tree_delete (head);
      free (session);
      if (er)
         return er;             // Error
   }

   w->rxptr = 0;                // next packet
   w->rxlen = 0;
   if (w->rxdata)
      free (w->rxdata);
   w->rxdata = NULL;
   {                            // Rx websocket packets
      while (1)
      {
         unsigned char head[14],
           hptr = 0,
            hlen = 2;
         ssize_t len = 0;
         while (hptr < hlen)
         {
            if (w->ss)
               len = SSL_read (w->ss, head + hptr, hlen - hptr);
            else
               len = recv (w->socket, head + hptr, hlen - hptr, 0);
            if (len <= 0)
               return NULL;     // closed
            hptr += len;
            if (hptr == 2)
            {                   // Work out header length
               if (head[1] & 0x80)
                  hlen += 4;    // mask
               int l = (head[1] & 0x7F);
               if (l == 126)
                  hlen += 2;    // len
               else if (l == 127)
                  hlen += 8;    // len
            }
         }
         if (websocket_debug)
         {
            fprintf (stderr, "Rx Header");
            for (hptr = 0; hptr < hlen; hptr++)
               fprintf (stderr, " %02X", head[hptr]);
            fprintf (stderr, "\n");
         }
         len = (head[1] & 0x7F);
         if (len == 126)
            len = (head[2] << 8) + (head[3]);
         else if (len == 127)
            len =
               ((unsigned long long) head[2] << 56) + ((unsigned long long) head[3] << 48) + ((unsigned long long) head[4] << 40) +
               ((unsigned long long) head[5] << 32) + ((unsigned long long) head[6] << 24) + ((unsigned long long) head[7] << 16) +
               ((unsigned long long) head[8] << 8) + ((unsigned long long) head[9]);
         w->rxdata = realloc (w->rxdata, (w->rxlen += len) + 1);
         while (w->rxptr < w->rxlen)
         {
            if (w->ss)
               len = SSL_read (w->ss, w->rxdata + w->rxptr, w->rxlen - w->rxptr);
            else
               len = recv (w->socket, w->rxdata + w->rxptr, w->rxlen - w->rxptr, 0);
            if (len <= 0)
               return NULL;     // closed
            size_t p = w->rxptr;
            w->rxptr += len;
            if (head[1] & 0x80)
            {                   // Mask
               int q = 0;
               while (p < w->rxptr)
               {
                  w->rxdata[p] ^= head[hlen - 4 + q];
                  q = ((q + 1) & 3);
                  p++;
               }
            }
         }
         if (head[0] & 0x80)
         {                      // End of data
            if (!(head[1] & 0x80))
               return "Unmasked data";
            if (websocket_debug)
            {
               fprintf (stderr, "Rx");
               if ((head[0] & 0x0F) == 1)
                  fprintf (stderr, " [%.*s]", (int) w->rxlen, w->rxdata);       // Text frame
               else
               {
                  unsigned int p;
                  for (p = 0; p < w->rxlen; p++)
                     fprintf (stderr, " %02X", w->rxdata[p]);   // Binary data
               }
               fprintf (stderr, "\n");
            }
            w->rxdata[w->rxlen] = 0;    // Always add a NULL for safety
            if ((head[0] & 0xF) == 1 || (head[0] & 0xF) == 2)
            {                   // data
               if (w->path && w->path->callbackraw)
               {                // Raw callback
                  if (websocket_debug)
                     fprintf (stderr, "%p Data callback\n", w);
                  char *e = w->path->callbackraw (w, NULL, w->rxptr, w->rxdata);
                  w->rxdata = NULL;     // Consumed
                  w->rxptr = 0;
                  if (e)
                     return e;  // bad
               } else if (w->path && w->path->callback)
               {                // JSON callback
                  xml_t xml = xml_tree_parse_json ((char *) w->rxdata, "json");
                  if (!xml)
                     return "Bad JSON";
                  if (websocket_debug)
                     fprintf (stderr, "%p Data callback\n", w);
                  char *e = w->path->callback (w, NULL, xml);   // XML is consumed
                  if (e)
                     return e;  // bad
               }
            } else if ((head[0] & 0xF) == 8)
            {                   // Close
               return NULL;
            } else if ((head[0] & 0xF) == 9)
            {                   // Ping
               head[0] = 0x8A;  // Send Pong
               if (head[1] & 0x80)
               {                // Reply is not masked
                  head[1] &= ~0x80;
                  hlen -= 4;
               }
               // Again, not quote txb_queue as we have the data
               txb_t *txb = malloc (sizeof (*txb));
               memset (txb, 0, sizeof (*txb));
               pthread_mutex_init (&txb->mutex, NULL);
               txb->count = 1;
               txb->len = w->rxlen;
               txb->buf = w->rxdata;
               memmove (txb->head, head, txb->hlen = hlen);
               w->rxdata = NULL;        // Used in this buffer
               txq_t *txq = malloc (sizeof (*txq));
               memset (txq, 0, sizeof (*txq));
               txq->data = txb;
               pthread_mutex_lock (&w->mutex);
               if (w->txq)
                  w->txe->next = txq;
               else
                  w->txq = txq;
               w->txe = txq;
               pthread_mutex_unlock (&w->mutex);
            } else if ((head[0] & 0xF) == 0xA)
            {                   // Pong
               struct timeval tv;
               struct timezone tz;
               gettimeofday (&tv, &tz);
               unsigned long long pong = tv.tv_sec * 1000000ULL + tv.tv_usec;;
               unsigned long long ping = 0;
               if (w->rxlen == sizeof (ping))
               {
                  memcpy (&ping, w->rxdata, sizeof (ping));
                  w->ping = pong - ping;
                  if (websocket_debug)
                     fprintf (stderr, "Pong %lluus\n", pong - ping);
               }
            }
            w->rxptr = 0;       // next packet
            w->rxlen = 0;
            if (w->rxdata)
            {
               free (w->rxdata);
               w->rxdata = NULL;
            }
         }
      }
   }

   return NULL;
}

void *
websocket_rx (void *p)
{                               // Rx thread
   sigignore (SIGPIPE);
   websocket_t *w = p;
   char *e = websocket_do_rx (w);
   if (e && websocket_debug)
      fprintf (stderr, "Rx socket response: %s\n", e);
   if (!w->connected)
   {                            // Error...
      char *res = NULL;
      size_t len = 0;
      if (*e == '@')
      {                         // Send a file!
         FILE *o = open_memstream (&res, &len);
         FILE *i = fopen (e + 1, "r");
         if (i)
         {
            fprintf (o, "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: ");
            char *p = strrchr (e + 1, '.') ? : ".plain";
            if (!strcasecmp (p, ".png"))
               fprintf (o, "image/png");
            else if (!strcasecmp (p, ".svg"))
               fprintf (o, "image/svg+xml");
            else if (!strcasecmp (p, ".js"))
               fprintf (o, "text/javascript");
            else
               fprintf (o, "text/%s", p + 1);
            fprintf (o, "\r\n\r\n");
            while (1)
            {
               char buf[10240];
               size_t l = fread (buf, 1, sizeof (buf), i);
               if (l <= 0)
                  break;
               fwrite (buf, l, 1, o);
            }
            fclose (i);
         } else
            fprintf (o, "HTTP/1.1 404 Not found\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nNot found");
         fclose (o);
      } else if (*e == '>')
         len = asprintf (&res, "HTTP/1.1 302 Moved\r\nLocation: %s\r\nConnection: close\r\n\r\n", e + 1);       // Redirect
      else if (*e == '*')
         len = asprintf (&res, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n%s", e + 1);    // General data
      else if (!strncmp (e, "204 ", 4))
         len = asprintf (&res, "HTTP/1.1 %s\r\nConnection: close\r\n\r\n", e);  // No content
      else if (!strncmp (e, "401 ", 4))
         len = asprintf (&res, "HTTP/1.1 401 Unauthorised\r\nWWW-Authenticate: Basic realm=\"%s\"\r\nConnection: close\r\n\r\nLogin required", e + 4);  // No content
      else if (isdigit (e[0]) && isdigit (e[1]) && isdigit (e[2]) && e[3] == ' ')       // Error message
         len = asprintf (&res, "HTTP/1.1 %s\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n%s", e, e + 4);
      else
         len = asprintf (&res, "HTTP/1.1 500 %s\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n%s", e, e);     // General error
      if (len > 0)
      {
         size_t ptr = 0;
         while (ptr < len)
         {
            size_t sent = 0;
            if (w->ss)
               sent = SSL_write (w->ss, res + ptr, len - ptr);
            else
               sent = send (w->socket, res + ptr, len - ptr, 0);
            if (sent <= 0)
               break;
            ptr += sent;
         }
      }
      free (res);
   }
   if (e && (*e == '*' || *e == '@' || *e == '>'))
      free (e);                 // Malloc'd
   pthread_mutex_lock (&w->mutex);
   if (w->pipe[1] >= 0)
      close (w->pipe[1]);       // stop tx
   w->pipe[1] = -1;
   pthread_mutex_unlock (&w->mutex);
   if (w->rxdata)
      free (w->rxdata);
   pthread_exit (NULL);
   return NULL;
}

void *
websocket_listen (void *p)
{                               // Listen thread
   sigignore (SIGPIPE);
   websocket_bind_t *b = p;
   while (1)
   {
      struct sockaddr_in6 addr = { 0 };
      socklen_t len = sizeof (addr);
      int s = accept (b->socket, (void *) &addr, &len);
      if (s < 0)
      {
         warn ("Bad accept");
         continue;
      }
      char from[INET6_ADDRSTRLEN + 1] = "";
      if (addr.sin6_family == AF_INET)
         inet_ntop (addr.sin6_family, &((struct sockaddr_in *) &addr)->sin_addr, from, sizeof (from));
      else
         inet_ntop (addr.sin6_family, &addr.sin6_addr, from, sizeof (from));
      if (!strncmp (from, "::ffff:", 7) && strchr (from, '.'))
         memmove (from, from + 7, strlen (from + 7) + 1);
      if (websocket_debug)
         fprintf (stderr, "Accepted connection from %s\n", from);
      websocket_t *w = malloc (sizeof (*w));
      if (!w)
      {
         warnx ("Malloc fail");
         close (s);
         continue;
      }
      memset (w, 0, sizeof (*w));
      pthread_mutex_init (&w->mutex, NULL);
      w->bind = b;
      w->socket = s;
      w->from = strdup (from);
      if (pipe ((int *) w->pipe))
      {                         // Failed to make pipe even, that is bad
         if (websocket_debug)
            fprintf (stderr, "Cannot make pipe\n");
         free (w);
         close (s);
         continue;
      }
      // Link in
      pthread_mutex_lock (&b->mutex);
      w->next = b->sessions;
      b->sessions = w;
      pthread_mutex_unlock (&b->mutex);
      // Threads (tx cleans up, so started last)
      pthread_t t;
      if (pthread_create (&t, NULL, websocket_rx, w))
      {                         // No rx task, close things and free
         if (websocket_debug)
            fprintf (stderr, "Cannot make rx thread\n");
         pthread_mutex_lock (&w->mutex);
         close (w->pipe[1]);    // Tells tx thread to give up and close/free
         w->pipe[1] = -1;
         pthread_mutex_unlock (&w->mutex);
         close (s);
         continue;
      }
      pthread_detach (t);
      if (pthread_create (&t, NULL, websocket_tx, w))
      {                         // Failed to make tx thread
         if (websocket_debug)
            fprintf (stderr, "Cannot make tx thread\n");
         pthread_mutex_lock (&w->mutex);
         close (w->pipe[0]);
         w->pipe[0] = -1;
         close (w->pipe[1]);
         w->pipe[1] = -1;
         pthread_mutex_unlock (&w->mutex);
         free (w);              // Problematic if rx task running.
         continue;
      }
      pthread_detach (t);
   }
   return NULL;
}

const char *
websocket_bind_base (const char *hostport, const char *origin, const char *host, const char *path, const char *certfile,
                     const char *keyfile, websocket_callback_t * cb, websocket_callback_raw_t * cbraw)
{
   signal (SIGPIPE, SIG_IGN);
   if (!hostport)
      hostport = (keyfile ? "https" : "http");
   websocket_bind_t *b;
   for (b = binds; b && strcmp (b->port, hostport); b = b->next);
   if (!b)
   {
      if (!binds)
         SSL_library_init ();
      // bind
      int s = -1;
      {                         // bind
         char *port = strdupa (hostport);
         char *host = NULL;
         char *c = strrchr (port, '#');
         if (c)
         {
            *c++ = 0;
            host = port;
            port = c;
         }
         if (websocket_debug)
            fprintf (stderr, "Bind [%s] %s\n", host ? : "*", port);
       const struct addrinfo hints = { ai_flags: AI_PASSIVE, ai_socktype: SOCK_STREAM, ai_family:AF_UNSPEC };
         struct addrinfo *res = NULL,
            *r;
         if (getaddrinfo (host, port, &hints, &res))
            return "Failed to get address info";
         if (!res)
            return "Cannot find port";
         const char *err = NULL;
         for (r = res; r && r->ai_family != AF_INET6; r = r->ai_next);
         if (!r)
            r = res;
         for (; r; r = r->ai_next)
         {
            s = socket (r->ai_family, r->ai_socktype, r->ai_protocol);
            if (s < 0)
            {
               err = "Cannot create socket";
               continue;
            }
            int on = 1;
            if (setsockopt (s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof (on)))
            {
               close (s);
               err = "Failed to set socket option (REUSE)";
               continue;
            }
            int max = MAXTCP;
            if (setsockopt (s, SOL_SOCKET, SO_RCVBUF, &max, sizeof (max)))
            {
               close (s);
               err = "Failed to set socket option (RCV)";
               continue;
            }
            if (setsockopt (s, SOL_SOCKET, SO_SNDBUF, &max, sizeof (max)))
            {
               close (s);
               err = "Failed to set socket option (SND)";
               continue;
            }
            if (bind (s, r->ai_addr, r->ai_addrlen))
            {
               close (s);
               err = "Failed to bind to address";
               continue;
            }
            if (listen (s, 10))
            {
               close (s);
               err = "Could not listen on port";
               continue;
            }
            // Worked
            err = NULL;
            break;
         }
         freeaddrinfo (res);
         if (err)
            return err;
      }
      // allocate
      b = malloc (sizeof (*b));
      memset (b, 0, sizeof (*b));
      pthread_mutex_init (&b->mutex, NULL);
      if (certfile)
         b->certfile = strdup (certfile);
      if (keyfile)
         b->keyfile = strdup (keyfile);
      b->port = strdup (hostport);
      b->socket = s;
      if (keyfile)
      {
         b->ctx = SSL_CTX_new (SSLv23_server_method ());        // Negotiates TLS
         if (!b->ctx)
            return "Cannot create SSL CTX";
         if (certfile)
         {
            int e = SSL_CTX_use_certificate_chain_file (b->ctx, certfile);
            if (e != 1)
               return "Cannot load cert file";
         }
         if (keyfile)
         {
            int e = SSL_CTX_use_PrivateKey_file (b->ctx, keyfile, SSL_FILETYPE_PEM);
            if (e != 1)
               return "Cannot load key file";
         }
      }
      b->next = binds;
      binds = b;
      pthread_t t;
      if (pthread_create (&t, NULL, websocket_listen, b))
         return "Thread create error";
      pthread_detach (t);
   } else if (strcmp (b->certfile ? : "", certfile ? : "") || strcmp (b->keyfile ? : "", keyfile ? : ""))
      return "Mismatched cert file on bind";
   websocket_path_t *p;
   for (p = b->paths;
        p && (strcmp (p->origin ? : "", origin ? : "") || strcmp (p->path ? : "", path ? : "")
              || strcmp (p->host ? : "", host ? : "")); p = p->next);
   if (p)
      return "Already bound";
   p = malloc (sizeof (*p));
   memset (p, 0, sizeof (*p));
   if (origin)
      p->origin = strdup (origin);
   if (host)
      p->host = strdup (host);
   if (path)
      p->path = strdup (path);
   p->callback = cb;
   p->callbackraw = cbraw;
   pthread_mutex_lock (&b->mutex);
   p->next = b->paths;
   b->paths = p;
   pthread_mutex_unlock (&b->mutex);
   return NULL;                 // OK
}

const char *
websocket_send_raw (int num, websocket_t ** w, size_t datalen, const unsigned char *data)
{                               // Send data to web sockets, send with NULL to close, does not consume data - data assumed to be malloc'd
   txb_t *txb = txb_new_data (datalen, data);
   int p;
   for (p = 0; p < num; p++)
      if (w[p])
         txb_queue (w[p], txb);
   txb_done (txb);              // Allows for initial set count to 1
   return NULL;
}

const char *
websocket_send (int num, websocket_t ** w, xml_t data)
{                               // Send data to web sockets, send with NULL to close, does not consume data
   txb_t *txb = txb_new (data);
   int p;
   for (p = 0; p < num; p++)
      if (w[p])
         txb_queue (w[p], txb);
   txb_done (txb);              // Allows for initial set count to 1
   return NULL;
}

const char *
websocket_send_all (xml_t data)
{                               // Send to all web sockets
   txb_t *txb = txb_new (data);
   websocket_bind_t *b;
   for (b = binds; b; b = b->next)
   {
      pthread_mutex_lock (&b->mutex);
      websocket_t *w;
      for (w = (websocket_t *) b->sessions; w; w = (websocket_t *) w->next)
         txb_queue (w, txb);
      pthread_mutex_unlock (&b->mutex);
   }
   txb_done (txb);              // Allows for initial set count to 1
   return NULL;
}

#ifdef	MAIN
int
main (int argc, const char *argv[])
{
   const char *origin = NULL;
   const char *host = NULL;
   const char *port = NULL;
   const char *path = NULL;
   const char *certfile = NULL;
   const char *keyfile = NULL;
   {                            // POPT
      poptContext optCon;       // context for parsing command-line options
      const struct poptOption optionsTable[] = {
         {"debug", 'v', POPT_ARG_NONE, &websocket_debug, 0, "Debug", NULL},
         {"cert-file", 'c', POPT_ARG_STRING, &certfile, 0, "Cert file", "filename"},
         {"key-file", 'k', POPT_ARG_STRING, &keyfile, 0, "Private key file", "filename"},
         {"origin", 'o', POPT_ARG_STRING, &origin, 0, "Origin", "hostname"},
         {"host", 'H', POPT_ARG_STRING, &host, 0, "Host", "hostname"},
         {"port", 'P', POPT_ARG_STRING, &port, 0, "Port", "name/number"},
         {"path", 'p', POPT_ARG_STRING, &path, 0, "Path", "URL path"},
         POPT_AUTOHELP {}
      };

      optCon = poptGetContext (NULL, argc, argv, optionsTable, 0);
      //poptSetOtherOptionHelp (optCon, "");

      int c;
      if ((c = poptGetNextOpt (optCon)) < -1)
         errx (1, "%s: %s\n", poptBadOption (optCon, POPT_BADOPTION_NOALIAS), poptStrerror (c));

      if (poptPeekArg (optCon))
      {
         poptPrintUsage (optCon, stderr, 0);
         return -1;
      }
      poptFreeContext (optCon);
   }

   char *called (websocket_t * w, xml_t head, xml_t data)
   {
      fprintf (stderr, "w=%p\n", w);
      if (head)
      {
         xml_write (stderr, head);
         xml_tree_delete (head);
      }
      if (data)
      {
         xml_write (stderr, data);
         xml_tree_delete (data);
      }
      if (!w)
         return strdup ("*Stupid test");
      return NULL;
   }
   const char *e = websocket_bind (port, origin, host, path, certfile, keyfile, called);
   if (e)
      errx (1, "Failed bind: %s", e);

   while (1);                   // Wait for shit to happen
   return 0;
}
#endif
