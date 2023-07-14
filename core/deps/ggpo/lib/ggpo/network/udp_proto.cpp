/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#include "udp_proto.h"

#include "../ggpo_types.h"
#include "bitvector.h"

static const int UDP_HEADER_SIZE = 28;     /* Size of IP + UDP headers */
static const int NUM_SYNC_PACKETS = 5;
static const int SYNC_RETRY_INTERVAL = 2000;
static const int SYNC_FIRST_RETRY_INTERVAL = 500;
static const int RUNNING_RETRY_INTERVAL = 200;
static const int KEEP_ALIVE_INTERVAL    = 200;
static const int QUALITY_REPORT_INTERVAL = 1000;
static const int NETWORK_STATS_INTERVAL  = 1000;
static const int UDP_SHUTDOWN_TIMER = 5000;
static const int MAX_SEQ_DISTANCE = (1 << 15);

UdpProtocol::UdpProtocol() :
   _udp(NULL),
   _peer_addr_len(0),
   _magic_number(0),
   _local_player_queue(-1),
   _queue(-1),
   _relay(false),
   _remote_magic_number(0),
   _connected(false),
   _round_trip_time(0),
   _packets_sent(0),
   _bytes_sent(0),
   _stats_start_time(0),
   _recv_packet_loss(0),
   _local_frame_advantage(0),
   _remote_frame_advantage(0),
   _last_send_time(0),
   _last_recv_time(0),
   _shutdown_timeout(0),
   _disconnect_event_sent(false),
   _disconnect_timeout(0),
   _disconnect_notify_start(0),
   _disconnect_notify_sent(false),
   _next_send_seq(0),
   _next_recv_seq(0)
{
   _last_sent_input.init(-1, NULL, 1);
   _last_received_input.init(-1, NULL, 1);
   _last_acked_input.init(-1, NULL, 1);

   memset(&_state, 0, sizeof _state);
   memset(_peer_connect_status, 0, sizeof(_peer_connect_status));
   for (unsigned i = 0; i < ARRAY_SIZE(_peer_connect_status); i++) {
      _peer_connect_status[i].last_frame = -1;
   }
   memset(&_peer_addr, 0, sizeof _peer_addr);
   _oo_packet.msg = NULL;

   _send_latency = GGPOPlatform::GetConfigInt("GGPO_NETWORK_DELAY");
   _oop_percent = GGPOPlatform::GetConfigInt("GGPO_OOP_PERCENT");
}

UdpProtocol::~UdpProtocol()
{
   ClearSendQueue();
}

void
UdpProtocol::Init(Udp *udp,
                  Poll &poll,
                  int queue,
                  char *ip,
                  u_short port,
                  bool relay, UdpMsg::connect_status *status)
{  
   _udp = udp;
   _queue = queue;
   _relay = relay;
   _local_connect_status = status;

   addrinfo* res = nullptr;
   addrinfo hints{};
   hints.ai_family = AF_UNSPEC; //  IPv4 or IPv6
   hints.ai_socktype = SOCK_DGRAM; // UDP
   hints.ai_flags = AI_NUMERICSERV; // service is port no
   char service[10] = {};
   snprintf(service, sizeof(service),"%d", port);
   const auto err = getaddrinfo(ip, service, &hints, &res);
   if (err != 0) {
      LogError("UdpProtocol::Init getaddrinfo error %d (ip:%s port:%d)", err, ip, port);
      return;
   }

   for (auto info = res; info != nullptr; info = info->ai_next) {
      if (info->ai_family == AF_INET || info->ai_family == AF_INET6) {
         memcpy(&_peer_addr, info->ai_addr, info->ai_addrlen);
         _peer_addr_len = info->ai_addrlen;
         break;
      }
   }

   freeaddrinfo(res);

   if (_peer_addr_len == 0) {
      LogError("UdpProtocol::Init no address found");
      return;
   }

   do {
      _magic_number = (uint16)rand();
   } while (_magic_number == 0);
   poll.RegisterLoop(this);
}

void
UdpProtocol::SendInput(GameInput &input)
{
   if (_udp) {
      if (_current_state == Running) {
         /*
          * Check to see if this is a good time to adjust for the rift...
          */
         _timesync.advance_frame(input, _local_frame_advantage, _remote_frame_advantage);

         /*
          * Save this input packet
          *
          * XXX: This queue may fill up for spectators who do not ack input packets in a timely
          * manner.  When this happens, we can either resize the queue (ug) or disconnect them
          * (better, but still ug).  For the meantime, make this queue really big to decrease
          * the odds of this happening...
          */
         _pending_output.push(input);
      }
      SendPendingOutput();
   }  
}

void
UdpProtocol::SendPendingOutput()
{
   UdpMsg *msg = new UdpMsg(UdpMsg::Input);
   int i, j, offset = 0;
   uint8 *bits;
   GameInput last;

   if (_pending_output.size()) {
      last = _last_acked_input;
      bits = msg->u.input.bits;

      msg->u.input.start_frame = _pending_output.front().frame;
      msg->u.input.input_size = (uint8)_pending_output.front().size;

      ASSERT(last.frame == -1 || last.frame + 1 == (int)msg->u.input.start_frame);
      for (j = 0; j < _pending_output.size(); j++) {
         GameInput &current = _pending_output.item(j);
         if (memcmp(current.bits, last.bits, current.size) != 0) {
            ASSERT((GAMEINPUT_MAX_BYTES * GAMEINPUT_MAX_PLAYERS * 8) < (1 << BITVECTOR_NIBBLE_SIZE));
            for (i = 0; i < current.size * 8; i++) {
               ASSERT(i < (1 << BITVECTOR_NIBBLE_SIZE));
               if (current.value(i) != last.value(i)) {
                  BitVector_SetBit(msg->u.input.bits, &offset);
                  (current.value(i) ? BitVector_SetBit : BitVector_ClearBit)(bits, &offset);
                  BitVector_WriteNibblet(bits, i, &offset);
               }
            }
         }
         BitVector_ClearBit(msg->u.input.bits, &offset);
         last = _last_sent_input = current;
      }
   } else {
      msg->u.input.start_frame = 0;
      msg->u.input.input_size = 0;
   }
   msg->u.input.ack_frame = _last_received_input.frame;
   msg->u.input.num_bits = (uint16)offset;

   msg->u.input.disconnect_requested = _current_state == Disconnected;
   if (_local_connect_status) {
      memcpy(msg->u.input.peer_connect_status, _local_connect_status, sizeof(UdpMsg::connect_status) * UDP_MSG_MAX_PLAYERS);
   } else {
      memset(msg->u.input.peer_connect_status, 0, sizeof(UdpMsg::connect_status) * UDP_MSG_MAX_PLAYERS);
   }

   ASSERT(offset < MAX_COMPRESSED_BITS);

   SendMsg(msg);
}

void
UdpProtocol::SendInputAck()
{
   UdpMsg *msg = new UdpMsg(UdpMsg::InputAck);
   msg->u.input_ack.ack_frame = _last_received_input.frame;
   SendMsg(msg);
}

bool
UdpProtocol::GetEvent(UdpProtocol::Event &e)
{
   if (_event_queue.size() == 0) {
      return false;
   }
   e = _event_queue.front();
   _event_queue.pop();
   return true;
}


bool
UdpProtocol::OnLoopPoll(void *cookie)
{
   if (!_udp) {
      return true;
   }

   unsigned int now = GGPOPlatform::GetCurrentTimeMS();
   unsigned int next_interval;

   PumpSendQueue();
   switch (_current_state) {
   case Syncing:
      next_interval = (_state.sync.roundtrips_remaining == NUM_SYNC_PACKETS) ? SYNC_FIRST_RETRY_INTERVAL : SYNC_RETRY_INTERVAL;
      if (_last_send_time && _last_send_time + next_interval < now && _peer_addr_len != 0) {
         Log("udpproto%d | No luck syncing after %d ms... Re-queueing sync packet.", _queue, next_interval);
         SendSyncRequest();
      }
      break;

   case Running:
      // xxx: rig all this up with a timer wrapper
      if (!_state.running.last_input_packet_recv_time || _state.running.last_input_packet_recv_time + RUNNING_RETRY_INTERVAL < now) {
         Log("udpproto%d | Haven't exchanged packets in a while (last received:%d  last sent:%d).  Resending.", _queue, _last_received_input.frame, _last_sent_input.frame);
         SendPendingOutput();
         _state.running.last_input_packet_recv_time = now;
      }

      if (!_state.running.last_quality_report_time || _state.running.last_quality_report_time + QUALITY_REPORT_INTERVAL < now) {
         UdpMsg *msg = new UdpMsg(UdpMsg::QualityReport);
         msg->u.quality_report.ping = GGPOPlatform::GetCurrentTimeMS();
         msg->u.quality_report.frame_advantage = (uint8)_local_frame_advantage;
         SendMsg(msg);
         _state.running.last_quality_report_time = now;
      }

      if (!_state.running.last_network_stats_interval || _state.running.last_network_stats_interval + NETWORK_STATS_INTERVAL < now) {
         UpdateNetworkStats();
         _state.running.last_network_stats_interval =  now;
      }

      if (_last_send_time && _last_send_time + KEEP_ALIVE_INTERVAL < now) {
         Log("udpproto%d | Sending keep alive packet", _queue);
         SendMsg(new UdpMsg(UdpMsg::KeepAlive));
      }

      if (_disconnect_timeout && _disconnect_notify_start && 
         !_disconnect_notify_sent && (_last_recv_time + _disconnect_notify_start < now)) {
         Log("udpproto%d | Endpoint has stopped receiving packets for %d ms.  Sending notification.", _queue, _disconnect_notify_start);
         Event e(Event::NetworkInterrupted);
         e.u.network_interrupted.disconnect_timeout = _disconnect_timeout - _disconnect_notify_start;
         QueueEvent(e);
         _disconnect_notify_sent = true;
      }

      if (_disconnect_timeout && (_last_recv_time + _disconnect_timeout < now)) {
         if (!_disconnect_event_sent) {
            Log("udpproto%d | Endpoint has stopped receiving packets for %d ms.  Disconnecting.", _queue, _disconnect_timeout);
            QueueEvent(Event(Event::Disconnected));
            _disconnect_event_sent = true;
         }
      }
      break;

   case Disconnected:
      if (_shutdown_timeout < now) {
         Log("udpproto%d | Shutting down udp connection.", _queue);
         _udp = NULL;
         _shutdown_timeout = 0;
      }
      break;

   default:
	  break;
   }


   return true;
}

void
UdpProtocol::Disconnect()
{
   _current_state = Disconnected;
   _shutdown_timeout = GGPOPlatform::GetCurrentTimeMS() + UDP_SHUTDOWN_TIMER;
}

void
UdpProtocol::SendSyncRequest()
{
   _state.sync.random = rand() & 0xFFFF;
   UdpMsg *msg = new UdpMsg(UdpMsg::SyncRequest);
   msg->u.sync_request.random_request = _state.sync.random;
   msg->verification_size = verification.size();
   if (!verification.empty())
	   memcpy(&msg->u.sync_request.verification[0], &verification[0], verification.size());
   SendMsg(msg);
}

void
UdpProtocol::SendMsg(UdpMsg *msg)
{
   LogMsg("send", msg);

   {
	   std::lock_guard<std::mutex> lock(_send_mutex);
	   _packets_sent++;
	   _last_send_time = GGPOPlatform::GetCurrentTimeMS();
	   _bytes_sent += msg->PacketSize();

	   msg->hdr.const_magic = CONST_MAGIC;
	   msg->hdr.magic = _magic_number;
	   msg->hdr.sequence_number = _next_send_seq++;
	   msg->hdr.remote_endpoint = _local_player_queue;

       if (_relay) {
          msg->hdr.relay_magic = RELAY_MAGIC;
          msg->hdr.relay_to_endpoint = _queue;
          msg->hdr.org_type = msg->hdr.type;
          msg->hdr.type = UdpMsg::MsgType::Relay;
       }

	   _send_queue.push(QueueEntry(GGPOPlatform::GetCurrentTimeMS(), _peer_addr, msg));
   }
   PumpSendQueue();
}

bool
UdpProtocol::HandlesMsg(sockaddr_storage &from, UdpMsg *msg)
{
   if (!_udp) {
      return false;
   }

   // FIXME: spectator may not work
   if (msg->hdr.remote_endpoint == _queue) {
       if (_peer_addr_len == 0) {
           _peer_addr = from;
           _peer_addr_len = sizeof(from); // XXX
       }
       return true;
   }

   return false;
}

void
UdpProtocol::OnMsg(UdpMsg *msg, int len)
{
   bool handled = false;
   typedef bool (UdpProtocol::*DispatchFn)(UdpMsg *msg, int len);
   static const DispatchFn table[] = {
      &UdpProtocol::OnInvalid,             /* Invalid */
      &UdpProtocol::OnSyncRequest,         /* SyncRequest */
      &UdpProtocol::OnSyncReply,           /* SyncReply */
      &UdpProtocol::OnInput,               /* Input */
      &UdpProtocol::OnQualityReport,       /* QualityReport */
      &UdpProtocol::OnQualityReply,        /* QualityReply */
      &UdpProtocol::OnKeepAlive,           /* KeepAlive */
      &UdpProtocol::OnInputAck,            /* InputAck */
      &UdpProtocol::OnAppData,             /* AppData */
   };

   if (msg->hdr.const_magic != CONST_MAGIC) {
      LogMsg("invalid const magic", msg);
      return;
   }

   // filter out messages that don't match what we expect
   uint16 seq = msg->hdr.sequence_number;
   if (msg->hdr.type != UdpMsg::SyncRequest &&
       msg->hdr.type != UdpMsg::SyncReply) {
      if (msg->hdr.magic != _remote_magic_number) {
         LogMsg("recv rejecting", msg);
         return;
      }

      // filter out out-of-order packets
      uint16 skipped = (uint16)((int)seq - (int)_next_recv_seq);
      // Log("udpproto%d | checking sequence number -> next - seq : %d - %d = %d", _queue, seq, _next_recv_seq, skipped);
      if (skipped > MAX_SEQ_DISTANCE) {
         Log("udpproto%d | dropping out of order packet (seq: %d, last seq:%d)", _queue, seq, _next_recv_seq);
         return;
      }

      if (_next_recv_seq < seq && skipped != 1) {
        _recv_packet_loss++;
      }
   }

   _next_recv_seq = seq;
   LogMsg("recv", msg);
   if (msg->hdr.type >= ARRAY_SIZE(table)) {
      OnInvalid(msg, len);
   } else {
      handled = (this->*(table[msg->hdr.type]))(msg, len);
   }
   if (handled) {
      _last_recv_time = GGPOPlatform::GetCurrentTimeMS();
      if (_disconnect_notify_sent && _current_state == Running) {
         QueueEvent(Event(Event::NetworkResumed));   
         _disconnect_notify_sent = false;
      }
   }
}

void
UdpProtocol::UpdateNetworkStats(void)
{
   int now = GGPOPlatform::GetCurrentTimeMS();

   if (_stats_start_time == 0) {
      _stats_start_time = now;
   }

   int total_bytes_sent = _bytes_sent + (UDP_HEADER_SIZE * _packets_sent);
   float seconds = (float)((now - _stats_start_time) / 1000.0);
   float Bps = total_bytes_sent / seconds;
   float udp_overhead = (float)(100.0 * (UDP_HEADER_SIZE * _packets_sent) / _bytes_sent);

   _kbps_sent = int(Bps / 1024);

   Log("udpproto%d | Network Stats -- Bandwidth: %.2f KBps   Packets Sent: %5d (%.2f pps)   "
       "KB Sent: %.2f    UDP Overhead: %.2f %%.", _queue,
       (float)_kbps_sent,
       _packets_sent,
       (float)_packets_sent * 1000 / (now - _stats_start_time),
       total_bytes_sent / 1024.0,
       udp_overhead);
}


void
UdpProtocol::QueueEvent(const UdpProtocol::Event &evt)
{
   LogEvent("Queuing event", evt);
   _event_queue.push(evt);
}

void
UdpProtocol::Synchronize()
{
   if (_udp) {
      _current_state = Syncing;
      _state.sync.roundtrips_remaining = NUM_SYNC_PACKETS;
      if (_peer_addr_len != 0)
    	  SendSyncRequest();
   }
}

bool
UdpProtocol::GetPeerConnectStatus(int id, int *frame)
{
   *frame = _peer_connect_status[id].last_frame;
   return !_peer_connect_status[id].disconnected;
}

void
UdpProtocol::LogMsg(const char *prefix, UdpMsg *msg)
{
   switch (msg->hdr.type) {
   case UdpMsg::SyncRequest:
      Log("udpproto%d | %s sync-request (%d).", _queue, prefix,
          msg->u.sync_request.random_request);
      break;
   case UdpMsg::SyncReply:
      Log("udpproto%d | %s sync-reply (%d).", _queue, prefix,
          msg->u.sync_reply.random_reply);
      break;
   case UdpMsg::QualityReport:
      Log("udpproto%d | %s quality report.", _queue, prefix);
      break;
   case UdpMsg::QualityReply:
      Log("udpproto%d | %s quality reply.", _queue, prefix);
      break;
   case UdpMsg::KeepAlive:
      Log("udpproto%d | %s keep alive.", _queue, prefix);
      break;
   case UdpMsg::Input:
      Log("udpproto%d | %s game-compressed-input %d (+ %d bits).", _queue, prefix, msg->u.input.start_frame, msg->u.input.num_bits);
      break;
   case UdpMsg::InputAck:
      Log("udpproto%d | %s input ack.", _queue, prefix);
      break;
   case UdpMsg::AppData:
      Log("udpproto%d | %s app data (%d bytes).", _queue, prefix, msg->u.app_data.size);
      break;
   default:
      Log("%s unknown type.\n", prefix);
      break;
   }
}

void
UdpProtocol::LogEvent(const char *prefix, const UdpProtocol::Event &evt)
{
   if (evt.type == UdpProtocol::Event::Synchronzied)
      Log("udpproto%d | %s (event: Synchronized).", _queue, prefix);
}

bool
UdpProtocol::OnInvalid(UdpMsg *msg, int len)
{
   ASSERT(false && "Invalid msg in UdpProtocol");
   return false;
}

bool
UdpProtocol::OnSyncRequest(UdpMsg *msg, int len)
{
   if (_remote_magic_number != 0 && msg->hdr.magic != _remote_magic_number) {
      Log("udpproto%d | Ignoring sync request from unknown endpoint (%d != %d).", _queue,
           msg->hdr.magic, _remote_magic_number);
      return false;
   }
   UdpMsg *reply = new UdpMsg(UdpMsg::SyncReply);
   reply->u.sync_reply.random_reply = msg->u.sync_request.random_request;
   // Calculate incoming verif data size
   msg->verification_size = 0;
   int msgVerifSize = len - msg->PacketSize();
   if (msgVerifSize != (int)verification.size()
		   || (msgVerifSize != 0 && memcmp(&msg->u.sync_request.verification[0], &verification[0], msgVerifSize)))
   {
	   Log("udpproto%d | Verification mismatch: size received %d expected %d", _queue, msgVerifSize, (int)verification.size());
	   reply->u.sync_reply.verification_failure = 1;
	   SendMsg(reply);
	   throw GGPOException("Verification mismatch", GGPO_ERRORCODE_VERIFICATION_ERROR);
   }
   // FIXME
   if (_state.sync.roundtrips_remaining == NUM_SYNC_PACKETS && msg->hdr.sequence_number == 0) {
      Log("udpproto%d | Sync request 0 received... Re-queueing sync packet.", _queue);
      SendSyncRequest();
   }

   reply->u.sync_reply.verification_failure = 0;
   SendMsg(reply);

   return true;
}

bool
UdpProtocol::OnSyncReply(UdpMsg *msg, int len)
{
   if (_current_state != Syncing) {
      Log("udpproto%d | Ignoring SyncReply while not synching.", _queue);
      return msg->hdr.magic == _remote_magic_number;
   }

   if (msg->u.sync_reply.random_reply != _state.sync.random) {
      Log("udpproto%d | sync reply %d != %d.  Keep looking...", _queue,
          msg->u.sync_reply.random_reply, _state.sync.random);
      return false;
   }
   if (msg->u.sync_reply.verification_failure == 1)
	   throw GGPOException("Peer reported verification failure", GGPO_ERRORCODE_VERIFICATION_ERROR);

   if (!_connected) {
      QueueEvent(Event(Event::Connected));
      _connected = true;
   }

   Log("udpproto%d | Checking sync state (%d round trips remaining).", _queue, _state.sync.roundtrips_remaining);
   if (--_state.sync.roundtrips_remaining == 0) {
      Log("udpproto%d | Synchronized!", _queue);
      QueueEvent(UdpProtocol::Event(UdpProtocol::Event::Synchronzied));
      _current_state = Running;
      _last_received_input.frame = -1;
      _remote_magic_number = msg->hdr.magic;
   } else {
      UdpProtocol::Event evt(UdpProtocol::Event::Synchronizing);
      evt.u.synchronizing.total = NUM_SYNC_PACKETS;
      evt.u.synchronizing.count = NUM_SYNC_PACKETS - _state.sync.roundtrips_remaining;
      QueueEvent(evt);
      SendSyncRequest();
   }
   return true;
}

bool
UdpProtocol::OnInput(UdpMsg *msg, int len)
{
   /*
    * If a disconnect is requested, go ahead and disconnect now.
    */
   bool disconnect_requested = msg->u.input.disconnect_requested;
   if (disconnect_requested) {
      if (_current_state != Disconnected && !_disconnect_event_sent) {
         Log("udpproto%d | Disconnecting endpoint on remote request.", _queue);
         QueueEvent(Event(Event::Disconnected));
         _disconnect_event_sent = true;
      }
   } else {
      /*
       * Update the peer connection status if this peer is still considered to be part
       * of the network.
       */
      UdpMsg::connect_status* remote_status = msg->u.input.peer_connect_status;
      for (unsigned i = 0; i < ARRAY_SIZE(_peer_connect_status); i++) {
         _peer_connect_status[i].disconnected |= remote_status[i].disconnected;
         if (!_peer_connect_status[i].disconnected) ASSERT(remote_status[i].last_frame >= _peer_connect_status[i].last_frame);
         _peer_connect_status[i].last_frame = MAX(_peer_connect_status[i].last_frame, remote_status[i].last_frame);
      }
   }

   /*
    * Decompress the input.
    */
   int last_received_frame_number = _last_received_input.frame;
   if (msg->u.input.num_bits) {
      int offset = 0;
      uint8 *bits = (uint8 *)msg->u.input.bits;
      int numBits = msg->u.input.num_bits;
      int currentFrame = msg->u.input.start_frame;

      _last_received_input.size = msg->u.input.input_size;
      if (_last_received_input.frame < 0) {
         _last_received_input.frame = msg->u.input.start_frame - 1;
      }
      while (offset < numBits) {
         /*
          * Keep walking through the frames (parsing bits) until we reach
          * the inputs for the frame right after the one we're on.
          */
         ASSERT(currentFrame <= (_last_received_input.frame + 1));
         bool useInputs = currentFrame == _last_received_input.frame + 1;

         while (BitVector_ReadBit(bits, &offset)) {
            int on = BitVector_ReadBit(bits, &offset);
            int button = BitVector_ReadNibblet(bits, &offset);
            if (useInputs) {
               if (on) {
                  _last_received_input.set(button);
               } else {
                  _last_received_input.clear(button);
               }
            }
         }
         ASSERT(offset <= numBits);

         /*
          * Now if we want to use these inputs, go ahead and send them to
          * the emulator.
          */
         if (useInputs) {
            /*
             * Move forward 1 frame in the stream.
             */
            char desc[1024];
            ASSERT(currentFrame == _last_received_input.frame + 1);
            _last_received_input.frame = currentFrame;

            /*
             * Send the event to the emulator
             */
            UdpProtocol::Event evt(UdpProtocol::Event::Input);
            evt.u.input.input = _last_received_input;

            if (LogTypes::LDEBUG <= MAX_LOGLEVEL)
            	_last_received_input.desc(desc, ARRAY_SIZE(desc));

            _state.running.last_input_packet_recv_time = GGPOPlatform::GetCurrentTimeMS();

            Log("udpproto%d | Sending frame %d to emu queue %d (%s).", _queue, _last_received_input.frame, _queue, desc);
            QueueEvent(evt);

         } else {
            Log("udpproto%d | Skipping past frame:(%d) current is %d.", _queue, currentFrame, _last_received_input.frame);
         }

         /*
          * Move forward 1 frame in the input stream.
          */
         currentFrame++;
      }
   }
   ASSERT(_last_received_input.frame >= last_received_frame_number);

   /*
    * Get rid of our buffered input
    */
   while (_pending_output.size() && _pending_output.front().frame < msg->u.input.ack_frame) {
      Log("udpproto%d | Throwing away pending output frame %d", _queue, _pending_output.front().frame);
      _last_acked_input = _pending_output.front();
      _pending_output.pop();
   }
   return true;
}


bool
UdpProtocol::OnInputAck(UdpMsg *msg, int len)
{
   /*
    * Get rid of our buffered input
    */
   while (_pending_output.size() && _pending_output.front().frame < msg->u.input_ack.ack_frame) {
      Log("udpproto%d | Throwing away pending output frame %d", _queue, _pending_output.front().frame);
      _last_acked_input = _pending_output.front();
      _pending_output.pop();
   }
   return true;
}

bool
UdpProtocol::OnQualityReport(UdpMsg *msg, int len)
{
   // send a reply so the other side can compute the round trip transmit time.
   UdpMsg *reply = new UdpMsg(UdpMsg::QualityReply);
   reply->u.quality_reply.pong = msg->u.quality_report.ping;
   SendMsg(reply);

   _remote_frame_advantage = msg->u.quality_report.frame_advantage;
   return true;
}

bool
UdpProtocol::OnQualityReply(UdpMsg *msg, int len)
{
   uint32 rtt = GGPOPlatform::GetCurrentTimeMS() - msg->u.quality_reply.pong;
   if (1000 < rtt) return true;
   _round_trip_time = _round_trip_time == 0 ? rtt : uint32(0.5 + 0.9 * _round_trip_time + 0.1 * rtt);
   return true;
}

bool
UdpProtocol::OnKeepAlive(UdpMsg *msg, int len)
{
   return true;
}

void
UdpProtocol::GetNetworkStats(struct GGPONetworkStats *s)
{
   s->network.ping = _round_trip_time;
   s->network.send_queue_len = _pending_output.size();
   s->network.kbps_sent = _kbps_sent;
   s->network.recv_packet_loss = _recv_packet_loss;
   s->timesync.remote_frames_behind = _remote_frame_advantage;
   s->timesync.local_frames_behind = _local_frame_advantage;
}

void
UdpProtocol::SetLocalFrameNumber(int localFrame)
{
   /*
    * Estimate which frame the other guy is one by looking at the
    * last frame they gave us plus some delta for the one-way packet
    * trip time.
    */
   int remoteFrame = _last_received_input.frame + (_round_trip_time * 60 + 2000) / 2000;

   /*
    * Our frame advantage is how many frames *behind* the other guy
    * we are.  Counter-intuative, I know.  It's an advantage because
    * it means they'll have to predict more often and our moves will
    * pop more frequenetly.
    */
   _local_frame_advantage = remoteFrame - localFrame;
}

void
UdpProtocol::SetLocalPlayerQueue(int queue)
{
    ASSERT(_local_player_queue == -1);
    _local_player_queue = queue;
}

int
UdpProtocol::RecommendFrameDelay()
{
   // XXX: require idle input should be a configuration parameter
   return _timesync.recommend_frame_wait_duration(false);
}


void
UdpProtocol::SetDisconnectTimeout(int timeout)
{
   _disconnect_timeout = timeout;
}

void
UdpProtocol::SetDisconnectNotifyStart(int timeout)
{
   _disconnect_notify_start = timeout;
}

void
UdpProtocol::PumpSendQueue()
{
   std::lock_guard<std::mutex> lock(_send_mutex);
   while (!_send_queue.empty()) {
      QueueEntry &entry = _send_queue.front();

      if (_send_latency) {
         // should really come up with a gaussian distributation based on the configured
         // value, but this will do for now.
         int jitter = (_send_latency * 2 / 3) + ((rand() % _send_latency) / 3);
         if ((int)GGPOPlatform::GetCurrentTimeMS() < _send_queue.front().queue_time + jitter) {
            break;
         }
      }
      if (_oop_percent && !_oo_packet.msg && ((rand() % 100) < _oop_percent)) {
         int delay = rand() % (_send_latency * 10 + 1000);
         Log("udpproto%d | creating rogue oop (seq: %d  delay: %d)", _queue, entry.msg->hdr.sequence_number, delay);
         _oo_packet.send_time = GGPOPlatform::GetCurrentTimeMS() + delay;
         _oo_packet.msg = entry.msg;
         _oo_packet.dest_addr = entry.dest_addr;
      } else {
         ASSERT(entry.dest_addr.ss_family == AF_INET || entry.dest_addr.ss_family == AF_INET6);

         _udp->SendTo((char *)entry.msg, entry.msg->PacketSize(), 0,
                      (struct sockaddr *)&entry.dest_addr, sizeof entry.dest_addr);

         delete entry.msg;
      }
      _send_queue.pop();
   }
   if (_oo_packet.msg && _oo_packet.send_time < (int)GGPOPlatform::GetCurrentTimeMS()) {
      Log("udpproto%d | sending rogue oop!", _queue);
      _udp->SendTo((char *)_oo_packet.msg, _oo_packet.msg->PacketSize(), 0,
                     (struct sockaddr *)&_oo_packet.dest_addr, sizeof _oo_packet.dest_addr);

      delete _oo_packet.msg;
      _oo_packet.msg = NULL;
   }
}

void
UdpProtocol::ClearSendQueue()
{
   std::lock_guard<std::mutex> lock(_send_mutex);
   while (!_send_queue.empty()) {
      delete _send_queue.front().msg;
      _send_queue.pop();
   }
}

void UdpProtocol::SendAppData(const void *data, int len, bool spectators)
{
	if (_udp == nullptr)
		return;
	if (_current_state != Synchronzied && _current_state != Running)
		return;

	UdpMsg *msg = new UdpMsg(UdpMsg::AppData);
	msg->u.app_data.spectators = spectators;
	msg->u.app_data.size = len;
	memcpy(msg->u.app_data.data, data, len);

	SendMsg(msg);
}

bool UdpProtocol::OnAppData(UdpMsg *msg, int len)
{
    UdpProtocol::Event evt(UdpProtocol::Event::AppData);
    evt.u.app_data.spectators = msg->u.app_data.spectators != 0;
    evt.u.app_data.size = msg->u.app_data.size;
    memcpy(evt.u.app_data.data, msg->u.app_data.data, msg->u.app_data.size);
    QueueEvent(evt);
	return true;
}

void UdpProtocol::SendUnmanagedMsg(UdpMsg* msg, int len)
{
	if (_udp == nullptr)
		return;

	if (_peer_addr_len != 0) {
		_udp->SendTo((char*)msg, len, 0, (struct sockaddr*)&_peer_addr, sizeof(_peer_addr));
	}
}
