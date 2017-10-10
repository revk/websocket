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

// Callback function
typedef char *websocket_callback_t (websocket_t*,xml_t head, xml_t data);     // return NULL if OK, else connection is closed/rejected
// The callback function is used in several ways. Where head/data are defined they are assumed to be consumed / freed by the callback
// Case			websocket_t	head	data
// WebSocket connect	Defined		Defined	NULL
// WebSocket receive	Defined		NULL	Defined
// WebSocket close	Defined		NULL	NULL
// HTTP GET		NULL		Defined	NULL
// HTTP POST		NULL		Defined	Defined/NULL	(a POST with no valid JSON calls with NULL data, see head name for "get"/"post")
//
// The head is an object of type "get", "post", etc. and has attributes for all headers sent (lower case) and all form style query args, and IP
// The "authorization" header is changed to base 64 decoded if it is Basic
//
// The response is typically a constant (non malloc) string with error message
// If the response starts with three digits and a space it is assumed to be an HTTP response
// If the response starts with a * it is assumed to be a malloc'd data response (after the *)
// If the response starts with a @ it is assumed to be a malloc'd filename to send (after the @)
// If the response starts with a > it is assumed to be a malloc'd redirect location (after the >)

// Binding is done by port, but this bind is then checked for origin, host, and path
// host, origin and path can be NULL to match any
// port can be NULL for 80/443
// keyfile means wss
// Return is NULL if OK, else error string
const char *websocket_bind (const char *port,const char *origin,const char *host,const char *path, const char *certfile,const char *keyfile, websocket_callback_t);
const char *websocket_send (int num,websocket_t**, xml_t);        // Send data to web sockets, send with NULL to close - entries allowed to be NULL to skip them
const char *websocket_send_all(xml_t data); // Send data to all web sockets.


// To help linking in
void *websocket_data(websocket_t*);
void websocket_set_data(websocket_t*,void*);
