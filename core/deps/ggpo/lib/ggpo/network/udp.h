/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#ifndef _UDP_H
#define _UDP_H

#include "ggpo_poll.h"
#include "udp_msg.h"
#include "ggponet.h"
#include "ring_buffer.h"

#define MAX_UDP_ENDPOINTS     16

constexpr size_t MAX_UDP_PACKET_SIZE = sizeof(UdpMsg);

class Udp : public IPollSink
{
public:
   struct Stats {
      int      bytes_sent;
      int      packets_sent;
      float    kbps_sent;
   };

   struct Callbacks {
      virtual ~Callbacks() { }
      virtual void OnMsg(sockaddr_storage &from, UdpMsg *msg, int len) = 0;
   };

   Udp();

   void Init(uint16 port, Poll *p, Callbacks *callbacks);
   
   void SendTo(char *buffer, int len, int flags, struct sockaddr *dst, int destlen);

   virtual bool OnLoopPoll(void *cookie);

public:
   ~Udp(void);

protected:
   // Network transmission information
   SOCKET         _socket_v4;
   SOCKET         _socket_v6;

   // state management
   Callbacks      *_callbacks;
};

#endif
