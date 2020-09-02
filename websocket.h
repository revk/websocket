// Web socket library

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

#ifdef	AXL_H
#ifndef	USEAXL
#define	USEAXL
#endif
#endif

#ifdef	AJL_H
#ifndef	USEAJL
#define	USEAJL
#endif
#endif

#ifdef	USEAXL
#include <axl.h>
#endif
#ifdef	USEAJL
#include <ajl.h>
#endif

typedef struct websocket_s websocket_t; // Handle for connected web sockets

// Callback function (raw functions pass len+data, otherwise object is parsed from JSON)
#ifdef	USEAXL
typedef char *websocket_callback_xml_t(websocket_t *, xml_t head, xml_t data);  // return NULL if OK, else connection is closed/rejected
typedef char *websocket_callback_xmlraw_t(websocket_t *, xml_t head, size_t datalen, const unsigned char *data);        // return NULL if OK, else connection is closed/rejected
#endif
#ifdef	USEAJL
typedef char *websocket_callback_json_t(websocket_t *, j_t head, j_t data);     // return NULL if OK, else connection is closed/rejected
typedef char *websocket_callback_jsonraw_t(websocket_t *, j_t head, size_t datalen, const unsigned char *data); // return NULL if OK, else connection is closed/rejected
#endif
// The callback function is used in several ways.
// IMPORTANT: Where head/data are defined they are assumed to be consumed / freed by the callback
// Case                 websocket_t     head    data
// WebSocket connect    Defined         Defined NULL
// WebSocket receive    Defined         NULL    Defined
// WebSocket close      Defined         NULL    NULL
// HTTP GET             NULL            Defined NULL
// HTTP POST            NULL            Defined Defined/NULL    (a POST with no valid JSON calls with NULL data, see head name for "get"/"post")
//
// Head contains
//  IP attribute with the IP address of the connection
//  query object if was a query string, with content being raw query and attributes of decoded name=value entries
//  http object with http header fields as attributes
//
// The response is typically a constant (non malloc) string with error message
// If the response starts with three digits and a space it is assumed to be an HTTP response
// If the response starts with a * it is assumed to be a malloc'd data response (after the *)
// If the response starts with a @ it is assumed to be a malloc'd filename to send (after the @)
// If the response starts with a > it is assumed to be a malloc'd redirect location (after the >)

// Binding is done by hostport, but this bind is then checked for origin, host, and path
// However hostport can be of the form hostname#port, which will try and bind to the hostname only (e.g. localhost)
// host, origin and path can be NULL to match any
// port can be NULL for 80/443
// keyfile means wss
// Return is NULL if OK, else error string
typedef struct {
   const char *port;
   const char *origin;
   const char *host;
   const char *path;
   const char *certfile;
   const char *keyfile;
#ifdef	USEAXL
   websocket_callback_xml_t *xml;
   websocket_callback_xmlraw_t *xmlraw;
#endif
#ifdef	USEAJL
   websocket_callback_json_t *json;
   websocket_callback_jsonraw_t *jsonraw;
#endif
} websocket_bindopts_t;
#define	websocket_bind(...) websocket_bind_opts((websocket_bindopts_t){__VA_ARGS__})
const char *websocket_bind_opts(websocket_bindopts_t);

typedef struct {
   int num;
   websocket_t **ws;
#ifdef	USEAXL
   xml_t xml;
#endif
#ifdef	USEAJL
   j_t json;
#endif
   size_t len;
   const unsigned char *data;
} websocket_send_t;
#define websocket_send(...) websocket_send_opts((websocket_send_t){__VA_ARGS__})
const char *websocket_send_opts(websocket_send_t);
// Send allows sending raw (if data/len set), or xml or json. If no raw, xml, or JSON, this is sending a close
// If num=0 and ws is NULL, this is send to all

unsigned long websocket_ping(websocket_t * w);  // Latest ping data (us)

// To help linking in
void *websocket_data(websocket_t *);
void websocket_set_data(websocket_t *, void *);
