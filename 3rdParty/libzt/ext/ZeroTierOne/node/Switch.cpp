/*
 * ZeroTier One - Network Virtualization Everywhere
 * Copyright (C) 2011-2019  ZeroTier, Inc.  https://www.zerotier.com/
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * --
 *
 * You can be released from the requirements of the license by purchasing
 * a commercial license. Buying such a license is mandatory as soon as you
 * develop commercial closed-source software that incorporates or links
 * directly against ZeroTier software without disclosing the source code
 * of your own application.
 */

#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <utility>
#include <stdexcept>

#include "../version.h"
#include "../include/ZeroTierOne.h"

#include "Constants.hpp"
#include "RuntimeEnvironment.hpp"
#include "Switch.hpp"
#include "Node.hpp"
#include "InetAddress.hpp"
#include "Topology.hpp"
#include "Peer.hpp"
#include "SelfAwareness.hpp"
#include "Packet.hpp"
#include "Trace.hpp"

namespace ZeroTier {

Switch::Switch(const RuntimeEnvironment *renv) :
	RR(renv),
	_lastBeaconResponse(0),
	_lastCheckedQueues(0),
	_lastUniteAttempt(8) // only really used on root servers and upstreams, and it'll grow there just fine
{
}

void Switch::onRemotePacket(void *tPtr,const int64_t localSocket,const InetAddress &fromAddr,const void *data,unsigned int len)
{
	try {
		const int64_t now = RR->node->now();

		const SharedPtr<Path> path(RR->topology->getPath(localSocket,fromAddr));
		path->received(now);

		if (len == 13) {
			/* LEGACY: before VERB_PUSH_DIRECT_PATHS, peers used broadcast
			 * announcements on the LAN to solve the 'same network problem.' We
			 * no longer send these, but we'll listen for them for a while to
			 * locate peers with versions <1.0.4. */

			const Address beaconAddr(reinterpret_cast<const char *>(data) + 8,5);
			if (beaconAddr == RR->identity.address())
				return;
			if (!RR->node->shouldUsePathForZeroTierTraffic(tPtr,beaconAddr,localSocket,fromAddr))
				return;
			const SharedPtr<Peer> peer(RR->topology->getPeer(tPtr,beaconAddr));
			if (peer) { // we'll only respond to beacons from known peers
				if ((now - _lastBeaconResponse) >= 2500) { // limit rate of responses
					_lastBeaconResponse = now;
					Packet outp(peer->address(),RR->identity.address(),Packet::VERB_NOP);
					outp.armor(peer->key(),true);
					path->send(RR,tPtr,outp.data(),outp.size(),now);
				}
			}

		} else if (len > ZT_PROTO_MIN_FRAGMENT_LENGTH) { // SECURITY: min length check is important since we do some C-style stuff below!
			if (reinterpret_cast<const uint8_t *>(data)[ZT_PACKET_FRAGMENT_IDX_FRAGMENT_INDICATOR] == ZT_PACKET_FRAGMENT_INDICATOR) {
				// Handle fragment ----------------------------------------------------

				Packet::Fragment fragment(data,len);
				const Address destination(fragment.destination());

				if (destination != RR->identity.address()) {
					if ( (!RR->topology->amUpstream()) && (!path->trustEstablished(now)) )
						return;

					if (fragment.hops() < ZT_RELAY_MAX_HOPS) {
						fragment.incrementHops();

						// Note: we don't bother initiating NAT-t for fragments, since heads will set that off.
						// It wouldn't hurt anything, just redundant and unnecessary.
						SharedPtr<Peer> relayTo = RR->topology->getPeer(tPtr,destination);
						if ((!relayTo)||(!relayTo->sendDirect(tPtr,fragment.data(),fragment.size(),now,false))) {
							// Don't know peer or no direct path -- so relay via someone upstream
							relayTo = RR->topology->getUpstreamPeer();
							if (relayTo)
								relayTo->sendDirect(tPtr,fragment.data(),fragment.size(),now,true);
						}
					}
				} else {
					// Fragment looks like ours
					const uint64_t fragmentPacketId = fragment.packetId();
					const unsigned int fragmentNumber = fragment.fragmentNumber();
					const unsigned int totalFragments = fragment.totalFragments();

					if ((totalFragments <= ZT_MAX_PACKET_FRAGMENTS)&&(fragmentNumber < ZT_MAX_PACKET_FRAGMENTS)&&(fragmentNumber > 0)&&(totalFragments > 1)) {
						// Fragment appears basically sane. Its fragment number must be
						// 1 or more, since a Packet with fragmented bit set is fragment 0.
						// Total fragments must be more than 1, otherwise why are we
						// seeing a Packet::Fragment?

						RXQueueEntry *const rq = _findRXQueueEntry(fragmentPacketId);
						Mutex::Lock rql(rq->lock);
						if (rq->packetId != fragmentPacketId) {
							// No packet found, so we received a fragment without its head.

							rq->timestamp = now;
							rq->packetId = fragmentPacketId;
							rq->frags[fragmentNumber - 1] = fragment;
							rq->totalFragments = totalFragments; // total fragment count is known
							rq->haveFragments = 1 << fragmentNumber; // we have only this fragment
							rq->complete = false;
						} else if (!(rq->haveFragments & (1 << fragmentNumber))) {
							// We have other fragments and maybe the head, so add this one and check

							rq->frags[fragmentNumber - 1] = fragment;
							rq->totalFragments = totalFragments;

							if (Utils::countBits(rq->haveFragments |= (1 << fragmentNumber)) == totalFragments) {
								// We have all fragments -- assemble and process full Packet

								for(unsigned int f=1;f<totalFragments;++f)
									rq->frag0.append(rq->frags[f - 1].payload(),rq->frags[f - 1].payloadLength());

								if (rq->frag0.tryDecode(RR,tPtr)) {
									rq->timestamp = 0; // packet decoded, free entry
								} else {
									rq->complete = true; // set complete flag but leave entry since it probably needs WHOIS or something
								}
							}
						} // else this is a duplicate fragment, ignore
					}
				}

				// --------------------------------------------------------------------
			} else if (len >= ZT_PROTO_MIN_PACKET_LENGTH) { // min length check is important!
				// Handle packet head -------------------------------------------------

				const Address destination(reinterpret_cast<const uint8_t *>(data) + 8,ZT_ADDRESS_LENGTH);
				const Address source(reinterpret_cast<const uint8_t *>(data) + 13,ZT_ADDRESS_LENGTH);

				if (source == RR->identity.address())
					return;

				if (destination != RR->identity.address()) {
					if ( (!RR->topology->amUpstream()) && (!path->trustEstablished(now)) && (source != RR->identity.address()) )
						return;

					Packet packet(data,len);

					if (packet.hops() < ZT_RELAY_MAX_HOPS) {
						packet.incrementHops();
						SharedPtr<Peer> relayTo = RR->topology->getPeer(tPtr,destination);
						if ((relayTo)&&(relayTo->sendDirect(tPtr,packet.data(),packet.size(),now,false))) {
							if ((source != RR->identity.address())&&(_shouldUnite(now,source,destination))) {
								const SharedPtr<Peer> sourcePeer(RR->topology->getPeer(tPtr,source));
								if (sourcePeer)
									relayTo->introduce(tPtr,now,sourcePeer);
							}
						} else {
							relayTo = RR->topology->getUpstreamPeer();
							if ((relayTo)&&(relayTo->address() != source)) {
								if (relayTo->sendDirect(tPtr,packet.data(),packet.size(),now,true)) {
									const SharedPtr<Peer> sourcePeer(RR->topology->getPeer(tPtr,source));
									if (sourcePeer)
										relayTo->introduce(tPtr,now,sourcePeer);
								}
							}
						}
					}
				} else if ((reinterpret_cast<const uint8_t *>(data)[ZT_PACKET_IDX_FLAGS] & ZT_PROTO_FLAG_FRAGMENTED) != 0) {
					// Packet is the head of a fragmented packet series

					const uint64_t packetId = (
						(((uint64_t)reinterpret_cast<const uint8_t *>(data)[0]) << 56) |
						(((uint64_t)reinterpret_cast<const uint8_t *>(data)[1]) << 48) |
						(((uint64_t)reinterpret_cast<const uint8_t *>(data)[2]) << 40) |
						(((uint64_t)reinterpret_cast<const uint8_t *>(data)[3]) << 32) |
						(((uint64_t)reinterpret_cast<const uint8_t *>(data)[4]) << 24) |
						(((uint64_t)reinterpret_cast<const uint8_t *>(data)[5]) << 16) |
						(((uint64_t)reinterpret_cast<const uint8_t *>(data)[6]) << 8) |
						((uint64_t)reinterpret_cast<const uint8_t *>(data)[7])
					);

					RXQueueEntry *const rq = _findRXQueueEntry(packetId);
					Mutex::Lock rql(rq->lock);
					if (rq->packetId != packetId) {
						// If we have no other fragments yet, create an entry and save the head

						rq->timestamp = now;
						rq->packetId = packetId;
						rq->frag0.init(data,len,path,now);
						rq->totalFragments = 0;
						rq->haveFragments = 1;
						rq->complete = false;
					} else if (!(rq->haveFragments & 1)) {
						// If we have other fragments but no head, see if we are complete with the head

						if ((rq->totalFragments > 1)&&(Utils::countBits(rq->haveFragments |= 1) == rq->totalFragments)) {
							// We have all fragments -- assemble and process full Packet

							rq->frag0.init(data,len,path,now);
							for(unsigned int f=1;f<rq->totalFragments;++f)
								rq->frag0.append(rq->frags[f - 1].payload(),rq->frags[f - 1].payloadLength());

							if (rq->frag0.tryDecode(RR,tPtr)) {
								rq->timestamp = 0; // packet decoded, free entry
							} else {
								rq->complete = true; // set complete flag but leave entry since it probably needs WHOIS or something
							}
						} else {
							// Still waiting on more fragments, but keep the head
							rq->frag0.init(data,len,path,now);
						}
					} // else this is a duplicate head, ignore
				} else {
					// Packet is unfragmented, so just process it
					IncomingPacket packet(data,len,path,now);
					if (!packet.tryDecode(RR,tPtr)) {
						RXQueueEntry *const rq = _nextRXQueueEntry();
						Mutex::Lock rql(rq->lock);
						rq->timestamp = now;
						rq->packetId = packet.packetId();
						rq->frag0 = packet;
						rq->totalFragments = 1;
						rq->haveFragments = 1;
						rq->complete = true;
					}
				}

				// --------------------------------------------------------------------
			}
		}
	} catch ( ... ) {} // sanity check, should be caught elsewhere
}

void Switch::onLocalEthernet(void *tPtr,const SharedPtr<Network> &network,const MAC &from,const MAC &to,unsigned int etherType,unsigned int vlanId,const void *data,unsigned int len)
{
	if (!network->hasConfig())
		return;

	// Check if this packet is from someone other than the tap -- i.e. bridged in
	bool fromBridged;
	if ((fromBridged = (from != network->mac()))) {
		if (!network->config().permitsBridging(RR->identity.address())) {
			RR->t->outgoingNetworkFrameDropped(tPtr,network,from,to,etherType,vlanId,len,"not a bridge");
			return;
		}
	}

	uint8_t qosBucket = ZT_QOS_DEFAULT_BUCKET;

	if (to.isMulticast()) {
		MulticastGroup multicastGroup(to,0);

		if (to.isBroadcast()) {
			if ( (etherType == ZT_ETHERTYPE_ARP) && (len >= 28) && ((((const uint8_t *)data)[2] == 0x08)&&(((const uint8_t *)data)[3] == 0x00)&&(((const uint8_t *)data)[4] == 6)&&(((const uint8_t *)data)[5] == 4)&&(((const uint8_t *)data)[7] == 0x01)) ) {
				/* IPv4 ARP is one of the few special cases that we impose upon what is
				 * otherwise a straightforward Ethernet switch emulation. Vanilla ARP
				 * is dumb old broadcast and simply doesn't scale. ZeroTier multicast
				 * groups have an additional field called ADI (additional distinguishing
			   * information) which was added specifically for ARP though it could
				 * be used for other things too. We then take ARP broadcasts and turn
				 * them into multicasts by stuffing the IP address being queried into
				 * the 32-bit ADI field. In practice this uses our multicast pub/sub
				 * system to implement a kind of extended/distributed ARP table. */
				multicastGroup = MulticastGroup::deriveMulticastGroupForAddressResolution(InetAddress(((const unsigned char *)data) + 24,4,0));
			} else if (!network->config().enableBroadcast()) {
				// Don't transmit broadcasts if this network doesn't want them
				RR->t->outgoingNetworkFrameDropped(tPtr,network,from,to,etherType,vlanId,len,"broadcast disabled");
				return;
			}
		} else if ((etherType == ZT_ETHERTYPE_IPV6)&&(len >= (40 + 8 + 16))) {
			// IPv6 NDP emulation for certain very special patterns of private IPv6 addresses -- if enabled
			if ((network->config().ndpEmulation())&&(reinterpret_cast<const uint8_t *>(data)[6] == 0x3a)&&(reinterpret_cast<const uint8_t *>(data)[40] == 0x87)) { // ICMPv6 neighbor solicitation
				Address v6EmbeddedAddress;
				const uint8_t *const pkt6 = reinterpret_cast<const uint8_t *>(data) + 40 + 8;
				const uint8_t *my6 = (const uint8_t *)0;

				// ZT-RFC4193 address: fdNN:NNNN:NNNN:NNNN:NN99:93DD:DDDD:DDDD / 88 (one /128 per actual host)

				// ZT-6PLANE address:  fcXX:XXXX:XXDD:DDDD:DDDD:####:####:#### / 40 (one /80 per actual host)
				// (XX - lower 32 bits of network ID XORed with higher 32 bits)

				// For these to work, we must have a ZT-managed address assigned in one of the
				// above formats, and the query must match its prefix.
				for(unsigned int sipk=0;sipk<network->config().staticIpCount;++sipk) {
					const InetAddress *const sip = &(network->config().staticIps[sipk]);
					if (sip->ss_family == AF_INET6) {
						my6 = reinterpret_cast<const uint8_t *>(reinterpret_cast<const struct sockaddr_in6 *>(&(*sip))->sin6_addr.s6_addr);
						const unsigned int sipNetmaskBits = Utils::ntoh((uint16_t)reinterpret_cast<const struct sockaddr_in6 *>(&(*sip))->sin6_port);
						if ((sipNetmaskBits == 88)&&(my6[0] == 0xfd)&&(my6[9] == 0x99)&&(my6[10] == 0x93)) { // ZT-RFC4193 /88 ???
							unsigned int ptr = 0;
							while (ptr != 11) {
								if (pkt6[ptr] != my6[ptr])
									break;
								++ptr;
							}
							if (ptr == 11) { // prefix match!
								v6EmbeddedAddress.setTo(pkt6 + ptr,5);
								break;
							}
						} else if (sipNetmaskBits == 40) { // ZT-6PLANE /40 ???
							const uint32_t nwid32 = (uint32_t)((network->id() ^ (network->id() >> 32)) & 0xffffffff);
							if ( (my6[0] == 0xfc) && (my6[1] == (uint8_t)((nwid32 >> 24) & 0xff)) && (my6[2] == (uint8_t)((nwid32 >> 16) & 0xff)) && (my6[3] == (uint8_t)((nwid32 >> 8) & 0xff)) && (my6[4] == (uint8_t)(nwid32 & 0xff))) {
								unsigned int ptr = 0;
								while (ptr != 5) {
									if (pkt6[ptr] != my6[ptr])
										break;
									++ptr;
								}
								if (ptr == 5) { // prefix match!
									v6EmbeddedAddress.setTo(pkt6 + ptr,5);
									break;
								}
							}
						}
					}
				}

				if ((v6EmbeddedAddress)&&(v6EmbeddedAddress != RR->identity.address())) {
					const MAC peerMac(v6EmbeddedAddress,network->id());

					uint8_t adv[72];
					adv[0] = 0x60; adv[1] = 0x00; adv[2] = 0x00; adv[3] = 0x00;
					adv[4] = 0x00; adv[5] = 0x20;
					adv[6] = 0x3a; adv[7] = 0xff;
					for(int i=0;i<16;++i) adv[8 + i] = pkt6[i];
					for(int i=0;i<16;++i) adv[24 + i] = my6[i];
					adv[40] = 0x88; adv[41] = 0x00;
					adv[42] = 0x00; adv[43] = 0x00; // future home of checksum
					adv[44] = 0x60; adv[45] = 0x00; adv[46] = 0x00; adv[47] = 0x00;
					for(int i=0;i<16;++i) adv[48 + i] = pkt6[i];
					adv[64] = 0x02; adv[65] = 0x01;
					adv[66] = peerMac[0]; adv[67] = peerMac[1]; adv[68] = peerMac[2]; adv[69] = peerMac[3]; adv[70] = peerMac[4]; adv[71] = peerMac[5];

					uint16_t pseudo_[36];
					uint8_t *const pseudo = reinterpret_cast<uint8_t *>(pseudo_);
					for(int i=0;i<32;++i) pseudo[i] = adv[8 + i];
					pseudo[32] = 0x00; pseudo[33] = 0x00; pseudo[34] = 0x00; pseudo[35] = 0x20;
					pseudo[36] = 0x00; pseudo[37] = 0x00; pseudo[38] = 0x00; pseudo[39] = 0x3a;
					for(int i=0;i<32;++i) pseudo[40 + i] = adv[40 + i];
					uint32_t checksum = 0;
					for(int i=0;i<36;++i) checksum += Utils::hton(pseudo_[i]);
					while ((checksum >> 16)) checksum = (checksum & 0xffff) + (checksum >> 16);
					checksum = ~checksum;
					adv[42] = (checksum >> 8) & 0xff;
					adv[43] = checksum & 0xff;

					RR->node->putFrame(tPtr,network->id(),network->userPtr(),peerMac,from,ZT_ETHERTYPE_IPV6,0,adv,72);
					return; // NDP emulation done. We have forged a "fake" reply, so no need to send actual NDP query.
				} // else no NDP emulation
			} // else no NDP emulation
		}

		// Check this after NDP emulation, since that has to be allowed in exactly this case
		if (network->config().multicastLimit == 0) {
			RR->t->outgoingNetworkFrameDropped(tPtr,network,from,to,etherType,vlanId,len,"multicast disabled");
			return;
		}

		/* Learn multicast groups for bridged-in hosts.
		 * Note that some OSes, most notably Linux, do this for you by learning
		 * multicast addresses on bridge interfaces and subscribing each slave.
		 * But in that case this does no harm, as the sets are just merged. */
		if (fromBridged)
			network->learnBridgedMulticastGroup(tPtr,multicastGroup,RR->node->now());

		// First pass sets noTee to false, but noTee is set to true in OutboundMulticast to prevent duplicates.
		if (!network->filterOutgoingPacket(tPtr,false,RR->identity.address(),Address(),from,to,(const uint8_t *)data,len,etherType,vlanId,qosBucket)) {
			RR->t->outgoingNetworkFrameDropped(tPtr,network,from,to,etherType,vlanId,len,"filter blocked");
			return;
		}

		RR->mc->send(
			tPtr,
			RR->node->now(),
			network,
			Address(),
			multicastGroup,
			(fromBridged) ? from : MAC(),
			etherType,
			data,
			len);
	} else if (to == network->mac()) {
		// Destination is this node, so just reinject it
		RR->node->putFrame(tPtr,network->id(),network->userPtr(),from,to,etherType,vlanId,data,len);
	} else if (to[0] == MAC::firstOctetForNetwork(network->id())) {
		// Destination is another ZeroTier peer on the same network

		Address toZT(to.toAddress(network->id())); // since in-network MACs are derived from addresses and network IDs, we can reverse this
		SharedPtr<Peer> toPeer(RR->topology->getPeer(tPtr,toZT));

		if (!network->filterOutgoingPacket(tPtr,false,RR->identity.address(),toZT,from,to,(const uint8_t *)data,len,etherType,vlanId,qosBucket)) {
			RR->t->outgoingNetworkFrameDropped(tPtr,network,from,to,etherType,vlanId,len,"filter blocked");
			return;
		}

		if (fromBridged) {
			Packet outp(toZT,RR->identity.address(),Packet::VERB_EXT_FRAME);
			outp.append(network->id());
			outp.append((unsigned char)0x00);
			to.appendTo(outp);
			from.appendTo(outp);
			outp.append((uint16_t)etherType);
			outp.append(data,len);
			if (!network->config().disableCompression() && !ZT_SDK)
				outp.compress();
			aqm_enqueue(tPtr,network,outp,true,qosBucket);
		} else {
			Packet outp(toZT,RR->identity.address(),Packet::VERB_FRAME);
			outp.append(network->id());
			outp.append((uint16_t)etherType);
			outp.append(data,len);
			if (!network->config().disableCompression() && !ZT_SDK)
				outp.compress();
			aqm_enqueue(tPtr,network,outp,true,qosBucket);
		}

	} else {
		// Destination is bridged behind a remote peer

		// We filter with a NULL destination ZeroTier address first. Filtrations
		// for each ZT destination are also done below. This is the same rationale
		// and design as for multicast.
		if (!network->filterOutgoingPacket(tPtr,false,RR->identity.address(),Address(),from,to,(const uint8_t *)data,len,etherType,vlanId,qosBucket)) {
			RR->t->outgoingNetworkFrameDropped(tPtr,network,from,to,etherType,vlanId,len,"filter blocked");
			return;
		}

		Address bridges[ZT_MAX_BRIDGE_SPAM];
		unsigned int numBridges = 0;

		/* Create an array of up to ZT_MAX_BRIDGE_SPAM recipients for this bridged frame. */
		bridges[0] = network->findBridgeTo(to);
		std::vector<Address> activeBridges(network->config().activeBridges());
		if ((bridges[0])&&(bridges[0] != RR->identity.address())&&(network->config().permitsBridging(bridges[0]))) {
			/* We have a known bridge route for this MAC, send it there. */
			++numBridges;
		} else if (!activeBridges.empty()) {
			/* If there is no known route, spam to up to ZT_MAX_BRIDGE_SPAM active
			 * bridges. If someone responds, we'll learn the route. */
			std::vector<Address>::const_iterator ab(activeBridges.begin());
			if (activeBridges.size() <= ZT_MAX_BRIDGE_SPAM) {
				// If there are <= ZT_MAX_BRIDGE_SPAM active bridges, spam them all
				while (ab != activeBridges.end()) {
					bridges[numBridges++] = *ab;
					++ab;
				}
			} else {
				// Otherwise pick a random set of them
				while (numBridges < ZT_MAX_BRIDGE_SPAM) {
					if (ab == activeBridges.end())
						ab = activeBridges.begin();
					if (((unsigned long)RR->node->prng() % (unsigned long)activeBridges.size()) == 0) {
						bridges[numBridges++] = *ab;
						++ab;
					} else ++ab;
				}
			}
		}

		for(unsigned int b=0;b<numBridges;++b) {
			if (network->filterOutgoingPacket(tPtr,true,RR->identity.address(),bridges[b],from,to,(const uint8_t *)data,len,etherType,vlanId,qosBucket)) {
				Packet outp(bridges[b],RR->identity.address(),Packet::VERB_EXT_FRAME);
				outp.append(network->id());
				outp.append((uint8_t)0x00);
				to.appendTo(outp);
				from.appendTo(outp);
				outp.append((uint16_t)etherType);
				outp.append(data,len);
				if (!network->config().disableCompression() && !ZT_SDK)
					outp.compress();
				aqm_enqueue(tPtr,network,outp,true,qosBucket);
			} else {
				RR->t->outgoingNetworkFrameDropped(tPtr,network,from,to,etherType,vlanId,len,"filter blocked (bridge replication)");
			}
		}
	}
}

void Switch::aqm_enqueue(void *tPtr, const SharedPtr<Network> &network, Packet &packet,bool encrypt,int qosBucket)
{
	if(!network->qosEnabled()) {
		send(tPtr, packet, encrypt);
		return;
	}
	NetworkQoSControlBlock *nqcb = _netQueueControlBlock[network->id()];
	if (!nqcb) {
		// DEBUG_INFO("creating network QoS control block (NQCB) for network %llx", network->id());
		nqcb = new NetworkQoSControlBlock();
		_netQueueControlBlock[network->id()] = nqcb;
		// Initialize ZT_QOS_NUM_BUCKETS queues and place them in the INACTIVE list
		// These queues will be shuffled between the new/old/inactive lists by the enqueue/dequeue algorithm
		for (int i=0; i<ZT_QOS_NUM_BUCKETS; i++) {
			nqcb->inactiveQueues.push_back(new ManagedQueue(i));
		}
	}

	if (packet.verb() != Packet::VERB_FRAME && packet.verb() != Packet::VERB_EXT_FRAME) {
		// DEBUG_INFO("skipping, no QoS for this packet, verb=%x", packet.verb());
		// just send packet normally, no QoS for ZT protocol traffic
		send(tPtr, packet, encrypt);
	} 

	_aqm_m.lock();

	// Enqueue packet and move queue to appropriate list

	const Address dest(packet.destination());
	TXQueueEntry *txEntry = new TXQueueEntry(dest,RR->node->now(),packet,encrypt);
	
	ManagedQueue *selectedQueue = nullptr;
	for (size_t i=0; i<ZT_QOS_NUM_BUCKETS; i++) {
		if (i < nqcb->oldQueues.size()) { // search old queues first (I think this is best since old would imply most recent usage of the queue)
			if (nqcb->oldQueues[i]->id == qosBucket) {
				selectedQueue = nqcb->oldQueues[i];
			}
		} if (i < nqcb->newQueues.size()) { // search new queues (this would imply not often-used queues)
			if (nqcb->newQueues[i]->id == qosBucket) {
				selectedQueue = nqcb->newQueues[i];
			}
		} if (i < nqcb->inactiveQueues.size()) { // search inactive queues
			if (nqcb->inactiveQueues[i]->id == qosBucket) {
				selectedQueue = nqcb->inactiveQueues[i];
				// move queue to end of NEW queue list
				selectedQueue->byteCredit = ZT_QOS_QUANTUM;
				// DEBUG_INFO("moving q=%p from INACTIVE to NEW list", selectedQueue);
				nqcb->newQueues.push_back(selectedQueue);
				nqcb->inactiveQueues.erase(nqcb->inactiveQueues.begin() + i);
			}
		}
	}
	if (!selectedQueue) {
		return;
	}

	selectedQueue->q.push_back(txEntry);
	selectedQueue->byteLength+=txEntry->packet.payloadLength();
	nqcb->_currEnqueuedPackets++;

	// DEBUG_INFO("nq=%2lu, oq=%2lu, iq=%2lu, nqcb.size()=%3d, bucket=%2d, q=%p", nqcb->newQueues.size(), nqcb->oldQueues.size(), nqcb->inactiveQueues.size(), nqcb->_currEnqueuedPackets, qosBucket, selectedQueue);

	// Drop a packet if necessary
	ManagedQueue *selectedQueueToDropFrom = nullptr;
	if (nqcb->_currEnqueuedPackets > ZT_QOS_MAX_ENQUEUED_PACKETS)
	{
		// DEBUG_INFO("too many enqueued packets (%d), finding packet to drop", nqcb->_currEnqueuedPackets);
		int maxQueueLength = 0;
		for (size_t i=0; i<ZT_QOS_NUM_BUCKETS; i++) {
			if (i < nqcb->oldQueues.size()) {
				if (nqcb->oldQueues[i]->byteLength > maxQueueLength) {
					maxQueueLength = nqcb->oldQueues[i]->byteLength;
					selectedQueueToDropFrom = nqcb->oldQueues[i];
				}
			} if (i < nqcb->newQueues.size()) {
				if (nqcb->newQueues[i]->byteLength > maxQueueLength) {
					maxQueueLength = nqcb->newQueues[i]->byteLength;
					selectedQueueToDropFrom = nqcb->newQueues[i];
				}
			} if (i < nqcb->inactiveQueues.size()) {
				if (nqcb->inactiveQueues[i]->byteLength > maxQueueLength) {
					maxQueueLength = nqcb->inactiveQueues[i]->byteLength;
					selectedQueueToDropFrom = nqcb->inactiveQueues[i];
				}
			}
		}
		if (selectedQueueToDropFrom) {
			// DEBUG_INFO("dropping packet from head of largest queue (%d payload bytes)", maxQueueLength);
			int sizeOfDroppedPacket = selectedQueueToDropFrom->q.front()->packet.payloadLength();
			delete selectedQueueToDropFrom->q.front();
			selectedQueueToDropFrom->q.pop_front();
			selectedQueueToDropFrom->byteLength-=sizeOfDroppedPacket;
			nqcb->_currEnqueuedPackets--;
		}
	}
	_aqm_m.unlock();
	aqm_dequeue(tPtr);
}

uint64_t Switch::control_law(uint64_t t, int count)
{
	return t + ZT_QOS_INTERVAL / sqrt(count);
}

Switch::dqr Switch::dodequeue(ManagedQueue *q, uint64_t now) 
{
	dqr r;
	r.ok_to_drop = false;
	r.p = q->q.front();

	if (r.p == NULL) {
		q->first_above_time = 0;
		return r;
	}
	uint64_t sojourn_time = now - r.p->creationTime;
	if (sojourn_time < ZT_QOS_TARGET || q->byteLength <= ZT_DEFAULT_MTU) {
		// went below - stay below for at least interval
		q->first_above_time = 0;
	} else {
		if (q->first_above_time == 0) {
			// just went above from below. if still above at
			// first_above_time, will say it's ok to drop.
			q->first_above_time = now + ZT_QOS_INTERVAL;
		} else if (now >= q->first_above_time) {
			r.ok_to_drop = true;
		}
	}
	return r;
}

Switch::TXQueueEntry * Switch::CoDelDequeue(ManagedQueue *q, bool isNew, uint64_t now)
{
	dqr r = dodequeue(q, now);

	if (q->dropping) {
		if (!r.ok_to_drop) {
			q->dropping = false;
		}
		while (now >= q->drop_next && q->dropping) {
			q->q.pop_front(); // drop
			r = dodequeue(q, now);
			if (!r.ok_to_drop) {
				// leave dropping state
				q->dropping = false;
			} else {
				++(q->count);
				// schedule the next drop.
				q->drop_next = control_law(q->drop_next, q->count);
			}
		}
	} else if (r.ok_to_drop) {
		q->q.pop_front(); // drop
		r = dodequeue(q, now);
		q->dropping = true;
		q->count = (q->count > 2 && now - q->drop_next < 8*ZT_QOS_INTERVAL)?
		q->count - 2 : 1;
		q->drop_next = control_law(now, q->count);
	}
	return r.p;
}

void Switch::aqm_dequeue(void *tPtr)
{
	// Cycle through network-specific QoS control blocks
	for(std::map<uint64_t,NetworkQoSControlBlock*>::iterator nqcb(_netQueueControlBlock.begin());nqcb!=_netQueueControlBlock.end();) {
		if (!(*nqcb).second->_currEnqueuedPackets) {
			return;
		}

		uint64_t now = RR->node->now();
		TXQueueEntry *entryToEmit = nullptr;
		std::vector<ManagedQueue*> *currQueues = &((*nqcb).second->newQueues);
		std::vector<ManagedQueue*> *oldQueues = &((*nqcb).second->oldQueues);
		std::vector<ManagedQueue*> *inactiveQueues = &((*nqcb).second->inactiveQueues);

		_aqm_m.lock();

		// Attempt dequeue from queues in NEW list
		bool examiningNewQueues = true;
		while (currQueues->size()) {
			ManagedQueue *queueAtFrontOfList = currQueues->front();
			if (queueAtFrontOfList->byteCredit < 0) {
				queueAtFrontOfList->byteCredit += ZT_QOS_QUANTUM;
				// Move to list of OLD queues
				// DEBUG_INFO("moving q=%p from NEW to OLD list", queueAtFrontOfList);
				oldQueues->push_back(queueAtFrontOfList);
				currQueues->erase(currQueues->begin());
			} else {
				entryToEmit = CoDelDequeue(queueAtFrontOfList, examiningNewQueues, now);
				if (!entryToEmit) {
					// Move to end of list of OLD queues
					// DEBUG_INFO("moving q=%p from NEW to OLD list", queueAtFrontOfList);
					oldQueues->push_back(queueAtFrontOfList);
					currQueues->erase(currQueues->begin());
				}
				else {
					int len = entryToEmit->packet.payloadLength();
					queueAtFrontOfList->byteLength -= len;
					queueAtFrontOfList->byteCredit -= len;
					// Send the packet!
					queueAtFrontOfList->q.pop_front();
					send(tPtr, entryToEmit->packet, entryToEmit->encrypt);
					(*nqcb).second->_currEnqueuedPackets--;
				}
				if (queueAtFrontOfList) {
					//DEBUG_INFO("dequeuing from q=%p, len=%lu in NEW list (byteCredit=%d)", queueAtFrontOfList, queueAtFrontOfList->q.size(), queueAtFrontOfList->byteCredit);
				}
				break;
			}
		}

		// Attempt dequeue from queues in OLD list
		examiningNewQueues = false;
		currQueues = &((*nqcb).second->oldQueues);
		while (currQueues->size()) {
			ManagedQueue *queueAtFrontOfList = currQueues->front();
			if (queueAtFrontOfList->byteCredit < 0) {
				queueAtFrontOfList->byteCredit += ZT_QOS_QUANTUM;
				oldQueues->push_back(queueAtFrontOfList);
				currQueues->erase(currQueues->begin());
			} else {
				entryToEmit = CoDelDequeue(queueAtFrontOfList, examiningNewQueues, now);
				if (!entryToEmit) {
					//DEBUG_INFO("moving q=%p from OLD to INACTIVE list", queueAtFrontOfList);
					// Move to inactive list of queues
					inactiveQueues->push_back(queueAtFrontOfList);
					currQueues->erase(currQueues->begin());
				}
				else {
					int len = entryToEmit->packet.payloadLength();
					queueAtFrontOfList->byteLength -= len;
					queueAtFrontOfList->byteCredit -= len;
					queueAtFrontOfList->q.pop_front();
					send(tPtr, entryToEmit->packet, entryToEmit->encrypt);
					(*nqcb).second->_currEnqueuedPackets--;
				}
				if (queueAtFrontOfList) {
					//DEBUG_INFO("dequeuing from q=%p, len=%lu in OLD list (byteCredit=%d)", queueAtFrontOfList, queueAtFrontOfList->q.size(), queueAtFrontOfList->byteCredit);
				}
				break;
			}
		}
		nqcb++;
		_aqm_m.unlock();
	}
}

void Switch::removeNetworkQoSControlBlock(uint64_t nwid)
{
	NetworkQoSControlBlock *nq = _netQueueControlBlock[nwid];
	if (nq) {
		_netQueueControlBlock.erase(nwid);
		delete nq;
		nq = NULL;
	}
}

void Switch::send(void *tPtr,Packet &packet,bool encrypt)
{
	const Address dest(packet.destination());
	if (dest == RR->identity.address())
		return;
	if (!_trySend(tPtr,packet,encrypt)) {
		{
			Mutex::Lock _l(_txQueue_m);
			if (_txQueue.size() >= ZT_TX_QUEUE_SIZE) {
				_txQueue.pop_front();
			}
			_txQueue.push_back(TXQueueEntry(dest,RR->node->now(),packet,encrypt));
		}
		if (!RR->topology->getPeer(tPtr,dest))
			requestWhois(tPtr,RR->node->now(),dest);
	}
}

void Switch::requestWhois(void *tPtr,const int64_t now,const Address &addr)
{
	if (addr == RR->identity.address())
		return;

	{
		Mutex::Lock _l(_lastSentWhoisRequest_m);
		int64_t &last = _lastSentWhoisRequest[addr];
		if ((now - last) < ZT_WHOIS_RETRY_DELAY)
			return;
		else last = now;
	}

	const SharedPtr<Peer> upstream(RR->topology->getUpstreamPeer());
	if (upstream) {
		Packet outp(upstream->address(),RR->identity.address(),Packet::VERB_WHOIS);
		addr.appendTo(outp);
		RR->node->expectReplyTo(outp.packetId());
		send(tPtr,outp,true);
	}
}

void Switch::doAnythingWaitingForPeer(void *tPtr,const SharedPtr<Peer> &peer)
{
	{
		Mutex::Lock _l(_lastSentWhoisRequest_m);
		_lastSentWhoisRequest.erase(peer->address());
	}

	const int64_t now = RR->node->now();
	for(unsigned int ptr=0;ptr<ZT_RX_QUEUE_SIZE;++ptr) {
		RXQueueEntry *const rq = &(_rxQueue[ptr]);
		Mutex::Lock rql(rq->lock);
		if ((rq->timestamp)&&(rq->complete)) {
			if ((rq->frag0.tryDecode(RR,tPtr))||((now - rq->timestamp) > ZT_RECEIVE_QUEUE_TIMEOUT))
				rq->timestamp = 0;
		}
	}

	{
		Mutex::Lock _l(_txQueue_m);
		for(std::list< TXQueueEntry >::iterator txi(_txQueue.begin());txi!=_txQueue.end();) {
			if (txi->dest == peer->address()) {
				if (_trySend(tPtr,txi->packet,txi->encrypt)) {
					_txQueue.erase(txi++);
				} else {
					++txi;
				}
			} else {
				++txi;
			}
		}
	}
}

unsigned long Switch::doTimerTasks(void *tPtr,int64_t now)
{
	const uint64_t timeSinceLastCheck = now - _lastCheckedQueues;
	if (timeSinceLastCheck < ZT_WHOIS_RETRY_DELAY)
		return (unsigned long)(ZT_WHOIS_RETRY_DELAY - timeSinceLastCheck);
	_lastCheckedQueues = now;

	std::vector<Address> needWhois;
	{
		Mutex::Lock _l(_txQueue_m);

		for(std::list< TXQueueEntry >::iterator txi(_txQueue.begin());txi!=_txQueue.end();) {
			if (_trySend(tPtr,txi->packet,txi->encrypt)) {
				_txQueue.erase(txi++);
			} else if ((now - txi->creationTime) > ZT_TRANSMIT_QUEUE_TIMEOUT) {
				_txQueue.erase(txi++);
			} else {
				if (!RR->topology->getPeer(tPtr,txi->dest))
					needWhois.push_back(txi->dest);
				++txi;
			}
		}
	}
	for(std::vector<Address>::const_iterator i(needWhois.begin());i!=needWhois.end();++i)
		requestWhois(tPtr,now,*i);

	for(unsigned int ptr=0;ptr<ZT_RX_QUEUE_SIZE;++ptr) {
		RXQueueEntry *const rq = &(_rxQueue[ptr]);
		Mutex::Lock rql(rq->lock);
		if ((rq->timestamp)&&(rq->complete)) {
			if ((rq->frag0.tryDecode(RR,tPtr))||((now - rq->timestamp) > ZT_RECEIVE_QUEUE_TIMEOUT)) {
				rq->timestamp = 0;
			} else {
				const Address src(rq->frag0.source());
				if (!RR->topology->getPeer(tPtr,src))
					requestWhois(tPtr,now,src);
			}
		}
	}

	{
		Mutex::Lock _l(_lastUniteAttempt_m);
		Hashtable< _LastUniteKey,uint64_t >::Iterator i(_lastUniteAttempt);
		_LastUniteKey *k = (_LastUniteKey *)0;
		uint64_t *v = (uint64_t *)0;
		while (i.next(k,v)) {
			if ((now - *v) >= (ZT_MIN_UNITE_INTERVAL * 8))
				_lastUniteAttempt.erase(*k);
		}
	}

	{
		Mutex::Lock _l(_lastSentWhoisRequest_m);
		Hashtable< Address,int64_t >::Iterator i(_lastSentWhoisRequest);
		Address *a = (Address *)0;
		int64_t *ts = (int64_t *)0;
		while (i.next(a,ts)) {
			if ((now - *ts) > (ZT_WHOIS_RETRY_DELAY * 2))
				_lastSentWhoisRequest.erase(*a);
		}
	}

	return ZT_WHOIS_RETRY_DELAY;
}

bool Switch::_shouldUnite(const int64_t now,const Address &source,const Address &destination)
{
	Mutex::Lock _l(_lastUniteAttempt_m);
	uint64_t &ts = _lastUniteAttempt[_LastUniteKey(source,destination)];
	if ((now - ts) >= ZT_MIN_UNITE_INTERVAL) {
		ts = now;
		return true;
	}
	return false;
}

bool Switch::_trySend(void *tPtr,Packet &packet,bool encrypt)
{
	SharedPtr<Path> viaPath;
	const int64_t now = RR->node->now();
	const Address destination(packet.destination());

	const SharedPtr<Peer> peer(RR->topology->getPeer(tPtr,destination));
	if (peer) {
		viaPath = peer->getAppropriatePath(now,false);
		if (!viaPath) {
			peer->tryMemorizedPath(tPtr,now); // periodically attempt memorized or statically defined paths, if any are known
			const SharedPtr<Peer> relay(RR->topology->getUpstreamPeer());
			if ( (!relay) || (!(viaPath = relay->getAppropriatePath(now,false))) ) {
				if (!(viaPath = peer->getAppropriatePath(now,true)))
					return false;
			}
		}
	} else {
		return false;
	}

	unsigned int mtu = ZT_DEFAULT_PHYSMTU;
	uint64_t trustedPathId = 0;
	RR->topology->getOutboundPathInfo(viaPath->address(),mtu,trustedPathId);

	unsigned int chunkSize = std::min(packet.size(),mtu);
	packet.setFragmented(chunkSize < packet.size());

	peer->recordOutgoingPacket(viaPath, packet.packetId(), packet.payloadLength(), packet.verb(), now);

	if (trustedPathId) {
		packet.setTrusted(trustedPathId);
	} else {
		packet.armor(peer->key(),encrypt);
	}

	if (viaPath->send(RR,tPtr,packet.data(),chunkSize,now)) {
		if (chunkSize < packet.size()) {
			// Too big for one packet, fragment the rest
			unsigned int fragStart = chunkSize;
			unsigned int remaining = packet.size() - chunkSize;
			unsigned int fragsRemaining = (remaining / (mtu - ZT_PROTO_MIN_FRAGMENT_LENGTH));
			if ((fragsRemaining * (mtu - ZT_PROTO_MIN_FRAGMENT_LENGTH)) < remaining)
				++fragsRemaining;
			const unsigned int totalFragments = fragsRemaining + 1;

			for(unsigned int fno=1;fno<totalFragments;++fno) {
				chunkSize = std::min(remaining,(unsigned int)(mtu - ZT_PROTO_MIN_FRAGMENT_LENGTH));
				Packet::Fragment frag(packet,fragStart,chunkSize,fno,totalFragments);
				viaPath->send(RR,tPtr,frag.data(),frag.size(),now);
				fragStart += chunkSize;
				remaining -= chunkSize;
			}
		}
	}

	return true;
}

} // namespace ZeroTier