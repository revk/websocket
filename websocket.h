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

typedef struct websocket_s websocket_t;        // Handle for connected web sockets

// Callback function (raw functions pass len+data, otherwise object is parsed from JSON and passed as an XML type)
typedef char *websocket_callback_t (websocket_t*,xml_t head, xml_t data);     // return NULL if OK, else connection is closed/rejected
typedef char *websocket_callback_raw_t (websocket_t*,xml_t head, size_t datalen,const unsigned char *data);     // return NULL if OK, else connection is closed/rejected
// The callback function is used in several ways. Where head/data are defined they are assumed to be consumed / freed by the callback
// Case			websocket_t	head	data
// WebSocket connect	Defined		Defined	NULL
// WebSocket receive	Defined		NULL	Defined
// WebSocket close	Defined		NULL	NULL
// HTTP GET		NULL		Defined	NULL
// HTTP POST		NULL		Defined	Defined/NULL	(a POST with no valid JSON calls with NULL data, see head name for "get"/"post")
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
const char *websocket_bind_base (const char *port,const char *origin,const char *host,const char *path, const char *certfile,const char *keyfile, websocket_callback_t*,websocket_callback_raw_t*);
#define websocket_bind(port,origin,host,path,cert,key,cb) websocket_bind_base(port,origin,host,path,cert,key,cb,NULL)
#define websocket_bind_raw(port,origin,host,path,cert,key,cb) websocket_bind_base(port,origin,host,path,cert,key,NULL,cb)
const char *websocket_send (int num,websocket_t**, xml_t);        // Send data to web sockets, send with NULL to close - entries allowed to be NULL to skip them
const char *websocket_send_raw (int num,websocket_t**, size_t datalen,const unsigned char *data);        // Send data to web sockets, send with NULL to close - entries allowed to be NULL to skip them
const char *websocket_send_all(xml_t data); // Send data to all web sockets.
unsigned long websocket_ping(websocket_t * w); // Latest ping data (us)

// To help linking in
void *websocket_data(websocket_t*);
void websocket_set_data(websocket_t*,void*);
