/* dhclient.c

   DHCP Client. */

/*
 * Copyright (c) 1995, 1996, 1997 The Internet Software Consortium.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of The Internet Software Consortium nor the names
 *    of its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INTERNET SOFTWARE CONSORTIUM AND
 * CONTRIBUTORS ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE INTERNET SOFTWARE CONSORTIUM OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This software has been written for the Internet Software Consortium
 * by Ted Lemon <mellon@fugue.com> in cooperation with Vixie
 * Enterprises.  To learn more about the Internet Software Consortium,
 * see ``http://www.vix.com/isc''.  To learn more about Vixie
 * Enterprises, see ``http://www.vix.com''.
 *
 * This client was substantially modified and enhanced by Elliot Poger
 * for use on Linux while he was working on the MosquitoNet project at
 * Stanford.
 *
 * The current version owes much to Elliot's Linux enhancements, but
 * was substantially reorganized and partially rewritten by Ted Lemon
 * so as to use the same networking framework that the Internet Software
 * Consortium DHCP server uses.   Much system-specific configuration code
 * was moved into a shell script so that as support for more operating
 * systems is added, it will not be necessary to port and maintain
 * system-specific configuration code to these operating systems - instead,
 * the shell script can invoke the native tools to accomplish the same
 * purpose.
 */

#ifndef lint
static char copyright[] =
"$Id: dhclient.c,v 1.29 1997/03/05 06:30:08 mellon Exp $ Copyright (c) 1995, 1996 The Internet Software Consortium.  All rights reserved.\n";
#endif /* not lint */

#include "dhcpd.h"

TIME cur_time;
TIME default_lease_time = 43200; /* 12 hours... */
TIME max_lease_time = 86400; /* 24 hours... */
struct tree_cache *global_options [256];

struct client_config top_level_config;

char *path_dhclient_conf = _PATH_DHCLIENT_CONF;
char *path_dhclient_db = _PATH_DHCLIENT_DB;
char *path_dhclient_pid = _PATH_DHCLIENT_PID;

int log_perror = 1;

struct iaddr iaddr_broadcast = { 4, { 255, 255, 255, 255 } };
struct iaddr iaddr_any = { 4, { 0, 0, 0, 0 } };
struct in_addr inaddr_any;
struct sockaddr_in sockaddr_broadcast;

/* ASSERT_STATE() does nothing now; it used to be
   assert (state_is == state_shouldbe). */
#define ASSERT_STATE(state_is, state_shouldbe) {}

#ifdef USE_FALLBACK
struct interface_info fallback_interface;
#endif

u_int16_t local_port;
u_int16_t remote_port;
int log_priority;
int no_daemon;

static void usage PROTO ((void));

int main (argc, argv, envp)
	int argc;
	char **argv, **envp;
{
	int i;
	struct servent *ent;
	struct interface_info *ip;

#ifdef SYSLOG_4_2
	openlog ("dhclient", LOG_NDELAY);
	log_priority = LOG_DAEMON;
#else
	openlog ("dhclient", LOG_NDELAY, LOG_DAEMON);
#endif

#if !(defined (DEBUG) || defined (SYSLOG_4_2))
	setlogmask (LOG_UPTO (LOG_INFO));
#endif	

	for (i = 1; i < argc; i++) {
		if (!strcmp (argv [i], "-p")) {
			if (++i == argc)
				usage ();
			local_port = htons (atoi (argv [i]));
			debug ("binding to user-specified port %d",
			       ntohs (local_port));
		} else if (!strcmp (argv [i], "-d")) {
			no_daemon = 1;
 		} else if (argv [i][0] == '-') {
 		    usage ();
 		} else {
 		    struct interface_info *tmp =
 			((struct interface_info *)
 			 dmalloc (sizeof *tmp, "specified_interface"));
 		    if (!tmp)
 			error ("Insufficient memory to %s %s",
 			       "record interface", argv [i]);
 		    memset (tmp, 0, sizeof *tmp);
 		    strcpy (tmp -> name, argv [i]);
 		    tmp -> next = interfaces;
 		    tmp -> flags = INTERFACE_REQUESTED;
 		    interfaces = tmp;
 		}
	}
	/* Default to the DHCP/BOOTP port. */
	if (!local_port) {
		ent = getservbyname ("dhcpc", "udp");
		if (!ent)
			local_port = htons (68);
		else
			local_port = ent -> s_port;
		endservent ();
	}
	remote_port = htons (ntohs (local_port) - 1);	/* XXX */
  
	/* Get the current time... */
	GET_TIME (&cur_time);

	sockaddr_broadcast.sin_family = AF_INET;
	sockaddr_broadcast.sin_port = remote_port;
	sockaddr_broadcast.sin_addr.s_addr = INADDR_BROADCAST;
#ifdef HAVE_SA_LEN
	sockaddr_broadcast.sin_len = sizeof sockaddr_broadcast;
#endif
	inaddr_any.s_addr = INADDR_ANY;

	/* Discover all the network interfaces. */
	discover_interfaces (DISCOVER_UNCONFIGURED);

	/* Parse the dhclient.conf file. */
	read_client_conf ();

	/* Parse the lease database. */
	read_client_leases ();

	/* Rewrite the lease database... */
	rewrite_client_leases ();

	/* If no broadcast interfaces were discovered, call the script
	   and tell it so. */
	if (!interfaces) {
		script_init ((struct interface_info *)0, "NBI",
			     (struct string_list *)0);
		script_go ((struct interface_info *)0);

		/* Nothing more to do. */
		exit (0);
	} else {
		/* Call the script with the list of interfaces. */
		for (ip = interfaces; ip; ip = ip -> next) {
			script_init (ip, "PREINIT", (struct string_list *)0);
			if (ip -> client -> alias)
				script_write_params (ip, "alias_",
						     ip -> client -> alias);
			script_go (ip);
		}
	}

	/* At this point, all the interfaces that the script thinks
	   are relevant should be running, so now we once again call
	   discover_interfaces(), and this time ask it to actually set
	   up the interfaces. */
	discover_interfaces (DISCOVER_RUNNING);

	/* Start a configuration state machine for each interface. */
	for (ip = interfaces; ip; ip = ip -> next) {
		srandom (cur_time + *(int *)(&ip -> hw_address.haddr [0]));
		ip -> client -> state = S_INIT;
		state_reboot (ip);
	}

	/* Start dispatching packets and timeouts... */
	dispatch (1);
	/*NOTREACHED*/
	return 0;
}

static void usage ()
{
	error ("Usage: dhclient [-c] [-p <port>] [interface]");
}

void cleanup ()
{
}

void relay (ip, packet, length)
	struct interface_info *ip;
	struct dhcp_packet *packet;
	int length;
{
}

/* Individual States:
 * 
 * Each routine is called from the dhclient_state_machine() in one of
 * these conditions:
 * -> entering INIT state
 * -> recvpacket_flag == 0: timeout in this state
 * -> otherwise: received a packet in this state
 *
 * Return conditions as handled by dhclient_state_machine():
 * Returns 1, sendpacket_flag = 1: send packet, reset timer.
 * Returns 1, sendpacket_flag = 0: just reset the timer (wait for a milestone).
 * Returns 0: finish the nap which was interrupted for no good reason.
 *
 * Several per-interface variables are used to keep track of the process:
 *   active_lease: the lease that is being used on the interface
 *                 (null pointer if not configured yet).
 *   offered_leases: leases corresponding to DHCPOFFER messages that have
 *		     been sent to us by DHCP servers.
 *   acked_leases: leases corresponding to DHCPACK messages that have been
 *		   sent to us by DHCP servers.
 *   sendpacket: DHCP packet we're trying to send.
 *   destination: IP address to send sendpacket to
 * In addition, there are several relevant per-lease variables.
 *   T1_expiry, T2_expiry, lease_expiry: lease milestones
 * In the active lease, these control the process of renewing the lease;
 * In leases on the acked_leases list, this simply determines when we
 * can no longer legitimately use the lease.
 */

void state_reboot (ip)
	struct interface_info *ip;
{
	/* If we don't remember an active lease, go straight to INIT. */
	if (!ip -> client -> active ||
	    ip -> client -> active -> rebind < cur_time) {
		state_init (ip);
		return;
	}

	/* We are in the rebooting state. */
	ip -> client -> state = S_REBOOTING;

	/* Make a DHCPREQUEST packet, and set appropriate per-interface
	   flags. */
	make_request (ip, ip -> client -> active);
	ip -> client -> xid = ip -> client -> packet.xid;
	ip -> client -> destination = iaddr_broadcast;
	ip -> client -> first_sending = cur_time;
	ip -> client -> interval = 0;

	/* Add an immediate timeout to cause the first DHCPDISCOVER packet
	   to go out. */
	send_request (ip);
}

/* Called when a lease has completely expired and we've been unable to
   renew it. */

void state_init (ip)
	struct interface_info *ip;
{
	ASSERT_STATE(state, S_INIT);

	/* Make a DHCPDISCOVER packet, and set appropriate per-interface
	   flags. */
	make_discover (ip, ip -> client -> active);
	ip -> client -> xid = ip -> client -> packet.xid;
	ip -> client -> destination = iaddr_broadcast;
	ip -> client -> state = S_SELECTING;
	ip -> client -> first_sending = cur_time;
	ip -> client -> interval = 0;

	/* Add an immediate timeout to cause the first DHCPDISCOVER packet
	   to go out. */
	send_discover (ip);
}

/* state_selecting is called when one or more DHCPOFFER packets have been
   received and a configurable period of time has passed. */

void state_selecting (ip)
	struct interface_info *ip;
{
	struct client_lease *lp, *next, *picked;

	ASSERT_STATE(state, S_SELECTING);

	/* Cancel state_selecting and send_discover timeouts, since either
	   one could have got us here. */
	cancel_timeout (state_selecting, ip);
	cancel_timeout (send_discover, ip);

	/* We have received one or more DHCPOFFER packets.   Currently,
	   the only criterion by which we judge leases is whether or
	   not we get a response when we arp for them. */
	picked = (struct client_lease *)0;
	for (lp = ip -> client -> offered_leases; lp; lp = next) {
		next = lp -> next;

		/* Check to see if we got an ARPREPLY for the address
		   in this particular lease. */
		if (!picked) {
			script_init (ip, "ARPCHECK", lp -> medium);
			script_write_params (ip, "check_", lp);

			/* If the ARPCHECK code detects another
			   machine using the offered address, it exits
			   nonzero.  We need to send a DHCPDECLINE and
			   toss the lease. */
			if (script_go (ip)) {
				make_decline (ip, lp);
				send_decline (ip);
				goto freeit;
			}
			picked = lp;
			picked -> next = (struct client_lease *)0;
		} else {
		      freeit:
			free_client_lease (lp);
		}
	}
	ip -> client -> offered_leases = (struct client_lease *)0;

	/* If we just tossed all the leases we were offered, go back
	   to square one. */
	if (!picked) {
		send_discover (ip);
		return;
	}

	/* Go to the REQUESTING state. */
	ip -> client -> destination = iaddr_broadcast;
	ip -> client -> state = S_REQUESTING;
	ip -> client -> first_sending = cur_time;
	ip -> client -> interval = 0;

	/* Make a DHCPREQUEST packet from the lease we picked. */
	make_request (ip, picked);
	ip -> client -> xid = ip -> client -> packet.xid;

	/* Toss the lease we picked - we'll get it back in a DHCPACK. */
	free_client_lease (picked);

	/* Add an immediate timeout to send the first DHCPREQUEST packet. */
	send_request (ip);
}  

/* state_requesting is called when we receive a DHCPACK message after
   having sent out one or more DHCPREQUEST packets. */

void dhcpack (packet)
	struct packet *packet;
{
	struct interface_info *ip = packet -> interface;
	struct client_lease *lease;
	int i;
	
	note ("DHCPACK from %s",
	      print_hw_addr (packet -> raw -> htype,
			     packet -> raw -> hlen,
			     packet -> raw -> chaddr));

	/* If we're not receptive to an offer right now, or if the offer
	   has an unrecognizable transaction id, then just drop it. */
	if (packet -> interface -> client -> xid != packet -> raw -> xid) {
		note ("DHCPACK in wrong transaction.");
		return;
	}

	if (ip -> client -> state != S_REBOOTING &&
	    ip -> client -> state != S_REQUESTING &&
	    ip -> client -> state != S_RENEWING &&
	    ip -> client -> state != S_REBINDING) {
		note ("DHCPACK in wrong state.");
		return;
	}

	lease = packet_to_lease (packet);
	if (!lease) {
		note ("packet_to_lease failed.");
		return;
	}

	ip -> client -> new = lease;

	/* Stop resending DHCPREQUEST. */
	cancel_timeout (send_request, ip);

	/* Figure out the lease time. */
	ip -> client -> new -> expiry =
		getULong (ip -> client ->
			  new -> options [DHO_DHCP_LEASE_TIME].data);

	/* Take the server-provided renewal time if there is one;
	   otherwise figure it out according to the spec. */
	if (ip -> client -> new -> options [DHO_DHCP_RENEWAL_TIME].len)
		ip -> client -> new -> renewal =
			getULong (ip -> client ->
				  new -> options [DHO_DHCP_RENEWAL_TIME].data);
	else
		ip -> client -> new -> renewal =
			ip -> client -> new -> expiry / 2;

	/* Same deal with the rebind time. */
	if (ip -> client -> new -> options [DHO_DHCP_REBINDING_TIME].len)
		ip -> client -> new -> rebind =
			getULong (ip -> client -> new ->
				  options [DHO_DHCP_REBINDING_TIME].data);
	else
		ip -> client -> new -> rebind =
			ip -> client -> new -> renewal +
				ip -> client -> new -> renewal / 2 +
					ip -> client -> new -> renewal / 4;

	ip -> client -> new -> expiry += cur_time;
	ip -> client -> new -> renewal += cur_time;
	ip -> client -> new -> rebind += cur_time;

	/* Write out the new lease. */
	write_client_lease (ip, ip -> client -> new);

	/* Remember the medium. */
	ip -> client -> new -> medium = ip -> client -> medium;

	/* Run the client script with the new parameters. */
	script_init (ip, (ip -> client -> state == S_REQUESTING
			  ? "BOUND"
			  : (ip -> client -> state == S_RENEWING
			     ? "RENEW"
			     : "REBIND")), ip -> client -> new -> medium);
	if (ip -> client -> active)
		script_write_params (ip, "old_", ip -> client -> active);
	script_write_params (ip, "new_", ip -> client -> new);
	if (ip -> client -> alias)
		script_write_params (ip, "alias_", ip -> client -> alias);
	script_go (ip);

	/* Replace the old active lease with the new one. */
	if (ip -> client -> active)
		free_client_lease (ip -> client -> active);
	ip -> client -> active = ip -> client -> new;
	ip -> client -> new = (struct client_lease *)0;

	/* Set up a timeout to start the renewal process. */
	add_timeout (ip -> client -> active -> renewal,
		     state_bound, ip);

	note ("bound: renewal in %d seconds.",
	      ip -> client -> active -> renewal - cur_time);
	ip -> client -> state = S_BOUND;
	reinitialize_interfaces ();
	go_daemon ();
}  

/* state_bound is called when we've successfully bound to a particular
   lease, but the renewal time on that lease has expired.   We are
   expected to unicast a DHCPREQUEST to the server that gave us our
   original lease. */

void state_bound (ip)
	struct interface_info *ip;
{
	ASSERT_STATE(state, S_BOUND);

	/* T1 has expired. */
	make_request (ip, ip -> client -> active);
	ip -> client -> xid = ip -> client -> packet.xid;

	if (ip -> client -> active ->
	    options [DHO_DHCP_SERVER_IDENTIFIER].len == 4) {
		memcpy (ip -> client -> destination.iabuf,
			ip -> client -> active ->
			options [DHO_DHCP_SERVER_IDENTIFIER].data, 4);
		ip -> client -> destination.len = 4;
	} else
		ip -> client -> destination = iaddr_broadcast;

	ip -> client -> first_sending = cur_time;
	ip -> client -> interval = 0;
	ip -> client -> state = S_RENEWING;

	/* Send the first packet immediately. */
	send_request (ip);
}  

int commit_leases ()
{
	return 0;
}

int write_lease (lease)
	struct lease *lease;
{
	return 0;
}

void db_startup ()
{
}

void bootp (packet)
	struct packet *packet;
{
	note ("BOOTREPLY from %s",
	      print_hw_addr (packet -> raw -> htype,
			     packet -> raw -> hlen,
			     packet -> raw -> chaddr));
}

void dhcp (packet)
	struct packet *packet;
{
	switch (packet -> packet_type) {
	      case DHCPOFFER:
		dhcpoffer (packet);
		break;

	      case DHCPNAK:
		dhcpnak (packet);
		break;

	      case DHCPACK:
		dhcpack (packet);
		break;

	      default:
		break;
	}
}

void dhcpoffer (packet)
	struct packet *packet;
{
	struct interface_info *ip = packet -> interface;
	struct client_lease *lease, *lp;
	int i;
	int arp_timeout_needed, stop_selecting;
	
	note ("DHCPOFFER from %s",
	      print_hw_addr (packet -> raw -> htype,
			     packet -> raw -> hlen,
			     packet -> raw -> chaddr));


#ifdef DEBUG_PACKET
	dump_packet (packet);
#endif	

	/* If we're not receptive to an offer right now, or if the offer
	   has an unrecognizable transaction id, then just drop it. */
	if (ip -> client -> state != S_SELECTING ||
	    packet -> interface -> client -> xid != packet -> raw -> xid) {
		note ("DHCPOFFER in wrong transaction.");
		return;
	}

	/* If this lease doesn't supply the minimum required parameters,
	   blow it off. */
	for (i = 0; ip -> client -> config -> required_options [i]; i++) {
		if (!packet -> options [ip -> client -> config ->
					required_options [i]].len) {
			note ("DHCPOFFER isn't satisfactory.");
			return;
		}
	}

	/* If we've already seen this lease, don't record it again. */
	for (lease = ip -> client -> offered_leases;
	     lease; lease = lease -> next) {
		if (lease -> address.len == sizeof packet -> raw -> yiaddr &&
		    !memcmp (lease -> address.iabuf,
			     &packet -> raw -> yiaddr, lease -> address.len)) {
			note ("DHCPOFFER already seen.");
			return;
		}
	}

	lease = packet_to_lease (packet);
	if (!lease) {
		note ("packet_to_lease failed.");
		return;
	}

	/* Record the medium under which this lease was offered. */
	lease -> medium = ip -> client -> medium;

	/* Send out an ARP Request for the offered IP address. */
	script_init (ip, "ARPSEND", lease -> medium);
	script_write_params (ip, "check_", lease);
	/* If the script can't send an ARP request without waiting, 
	   we'll be waiting when we do the ARPCHECK, so don't wait now. */
	if (script_go (ip))
		arp_timeout_needed = 0;
	else
		arp_timeout_needed = 2;

	/* Figure out when we're supposed to stop selecting. */
	stop_selecting = (ip -> client -> first_sending +
			  ip -> client -> config -> select_interval);

	/* If this is the lease we asked for, put it at the head of the
	   list, and don't mess with the arp request timeout. */
	if (lease -> address.len == ip -> client -> requested_address.len &&
	    !memcmp (lease -> address.iabuf,
		     ip -> client -> requested_address.iabuf,
		     ip -> client -> requested_address.len)) {
		lease -> next = ip -> client -> offered_leases;
		ip -> client -> offered_leases = lease;
	} else {
		/* If we already have an offer, and arping for this
		   offer would take us past the selection timeout,
		   then don't extend the timeout - just hope for the
		   best. */
		if (ip -> client -> offered_leases &&
		    (cur_time + arp_timeout_needed) > stop_selecting)
			arp_timeout_needed = 0;

		/* Put the lease at the end of the list. */
		lease -> next = (struct client_lease *)0;
		if (!ip -> client -> offered_leases)
			ip -> client -> offered_leases = lease;
		else {
			for (lp = ip -> client -> offered_leases; lp -> next;
			     lp = lp -> next)
				;
			lp -> next = lease;
		}
	}

	/* If we're supposed to stop selecting before we've had time
	   to wait for the ARPREPLY, add some delay to wait for
	   the ARPREPLY. */
	if (stop_selecting - cur_time < arp_timeout_needed)
		stop_selecting = cur_time + arp_timeout_needed;

	/* If the selecting interval has expired, go immediately to
	   state_selecting().  Otherwise, time out into
	   state_selecting at the select interval. */
	if (stop_selecting <= 0)
		state_selecting (ip);
	else
		add_timeout (stop_selecting, state_selecting, ip);
}

/* Allocate a client_lease structure and initialize it from the parameters
   in the specified packet. */

struct client_lease *packet_to_lease (packet)
	struct packet *packet;
{
	struct client_lease *lease;
	int i;

	lease = (struct client_lease *)malloc (sizeof (struct client_lease));

	if (!lease) {
		warn ("dhcpoffer: no memory to record lease.\n");
		return (struct client_lease *)0;
	}

	memset (lease, 0, sizeof *lease);

	/* Copy the lease options. */
	for (i = 0; i < 256; i++) {
		if (packet -> options [i].len) {
			lease -> options [i].data =
				(unsigned char *)
					malloc (packet -> options [i].len);
			if (!lease -> options [i].data) {
				warn ("dhcpoffer: no memory for option %d\n",
				      i);
				free_client_lease (lease);
				return (struct client_lease *)0;
			} else {
				memcpy (lease -> options [i].data,
					packet -> options [i].data,
					packet -> options [i].len);
				lease -> options [i].len =
					packet -> options [i].len;
			}
		}
	}

	lease -> address.len = sizeof (packet -> raw -> yiaddr);
	memcpy (lease -> address.iabuf, &packet -> raw -> yiaddr,
		lease -> address.len);

	/* If the server name was filled out, copy it. */
	if ((!packet -> options [DHO_DHCP_OPTION_OVERLOAD].len ||
	     !(packet -> options [DHO_DHCP_OPTION_OVERLOAD].data [0] & 2)) &&
	    packet -> raw -> sname [0]) {
		int len;
		/* Don't count on the NUL terminator. */
		for (len = 0; len < 64; len++)
			if (!packet -> raw -> sname [len])
				break;
		lease -> server_name = malloc (len + 1);
		if (!lease -> server_name) {
			warn ("dhcpoffer: no memory for filename.\n");
			free_client_lease (lease);
			return (struct client_lease *)0;
		} else {
			memcpy (lease -> server_name,
				packet -> raw -> sname, len);
			lease -> server_name [len] = 0;
		}
	}

	/* Ditto for the filename. */
	if ((!packet -> options [DHO_DHCP_OPTION_OVERLOAD].len ||
	     !(packet -> options [DHO_DHCP_OPTION_OVERLOAD].data [0] & 1)) &&
	    packet -> raw -> file [0]) {
		int len;
		/* Don't count on the NUL terminator. */
		for (len = 0; len < 64; len++)
			if (!packet -> raw -> file [len])
				break;
		lease -> filename = malloc (len + 1);
		if (!lease -> filename) {
			warn ("dhcpoffer: no memory for filename.\n");
			free_client_lease (lease);
			return (struct client_lease *)0;
		} else {
			memcpy (lease -> filename,
				packet -> raw -> file, len);
			lease -> filename [len] = 0;
		}
	}
	return lease;
}	

void dhcpnak (packet)
	struct packet *packet;
{
	struct interface_info *ip = packet -> interface;

	note ("DHCPNAK from %s",
	      print_hw_addr (packet -> raw -> htype,
			     packet -> raw -> hlen,
			     packet -> raw -> chaddr));

	/* If we're not receptive to an offer right now, or if the offer
	   has an unrecognizable transaction id, then just drop it. */
	if (packet -> interface -> client -> xid != packet -> raw -> xid) {
		note ("DHCPNAK in wrong transaction.");
		return;
	}

	if (ip -> client -> state != S_REBOOTING &&
	    ip -> client -> state != S_REQUESTING &&
	    ip -> client -> state != S_RENEWING &&
	    ip -> client -> state != S_REBINDING) {
		note ("DHCPNAK in wrong state.");
		return;
	}

	if (!ip -> client -> active) {
		note ("DHCPNAK with no active lease.\n");
		return;
	}

	free_client_lease (ip -> client -> active);
	ip -> client -> active = (struct client_lease *)0;

	/* Stop sending DHCPREQUEST packets... */
	cancel_timeout (send_request, ip);

	ip -> client -> state = S_INIT;
	state_init (ip);
}

/* Send out a DHCPDISCOVER packet, and set a timeout to send out another
   one after the right interval has expired.   If we are past the renewal
   (T1) interval but not yet past the rebind (T2) interval, unicast
   the message; otherwise broadcast it.   If the lease expires, go back to
   the INIT state. */

void send_discover (ip)
	struct interface_info *ip;
{
	int result;
	int interval;
	int increase = 1;

	/* Figure out how long it's been since we started transmitting. */
	interval = cur_time - ip -> client -> first_sending;

	/* If we're past the panic timeout, call the script and tell it
	   we haven't found anything for this interface yet. */
	if (interval > ip -> client -> config -> timeout) {
		state_panic (ip);
		return;
	}

	/* If we're selecting media, try the whole list before doing
	   the exponential backoff, but if we've already received an
	   offer, stop looping, because we obviously have it right. */
	if (!ip -> client -> offered_leases &&
	    ip -> client -> config -> media) {
		int fail = 0;
	      again:
		if (ip -> client -> medium) {
			ip -> client -> medium =
				ip -> client -> medium -> next;
			increase = 0;
		} 
		if (!ip -> client -> medium) {
			if (fail)
				error ("No valid media types for %s!",
				       ip -> name);
			ip -> client -> medium =
				ip -> client -> config -> media;
			increase = 1;
		}
			
		note ("Trying medium \"%s\" %d",
		      ip -> client -> medium -> string, increase);
		script_init (ip, "MEDIUM", ip -> client -> medium);
		if (script_go (ip)) {
			goto again;
		}
	}

	/* If we're supposed to increase the interval, do so.  If it's
	   currently zero (i.e., we haven't sent any packets yet), set
	   it to one; otherwise, add to it a random number between
	   zero and two times itself.  On average, this means that it
	   will double with every transmission. */
	if (increase) {
		if (!ip -> client -> interval)
			ip -> client -> interval = 1;
		else {
			ip -> client -> interval +=
				random () % (2 * ip -> client -> interval);
		}

		/* Don't backoff past 30 seconds. */
		if (ip -> client -> interval > 30)
			ip -> client -> interval =
				15 + random () % ip -> client -> interval;
	} else if (!ip -> client -> interval)
		ip -> client -> interval = 1;
		
	/* If the backoff would take us to the panic timeout, just use that
	   as the interval. */
	if (cur_time + ip -> client -> interval >
	    ip -> client -> first_sending + ip -> client -> config -> timeout)
		ip -> client -> interval =
			(ip -> client -> first_sending +
			 ip -> client -> config -> timeout) - cur_time + 1;

	note ("DHCPDISCOVER on %s to %s port %d interval %ld",
	      ip -> name,
	      inet_ntoa (sockaddr_broadcast.sin_addr),
	      ntohs (sockaddr_broadcast.sin_port), ip -> client -> interval);

	/* Send out a packet. */
	result = send_packet (ip, (struct packet *)0,
			      &ip -> client -> packet,
			      ip -> client -> packet_length,
			      inaddr_any, &sockaddr_broadcast,
			      (struct hardware *)0);
	if (result < 0)
		warn ("send_packet: %m");

	add_timeout (cur_time + ip -> client -> interval, send_discover, ip);
}

/* state_panic gets called if we haven't received any offers in a preset
   amount of time.   When this happens, we try to use existing leases that
   haven't yet expired, and failing that, we call the client script and
   hope it can do something. */

void state_panic (ip)
	struct interface_info *ip;
{
	struct client_lease *loop = ip -> client -> active;
	struct client_lease *lp;

	note ("No DHCPOFFERS received.");

	/* We may not have an active lease, but we may have some
	   predefined leases that we can try. */
	if (!ip -> client -> active && ip -> client -> leases) {
		loop = ip -> client -> leases;
		goto activate_next;
	}

	/* Run through the list of leases and see if one can be used. */
	while (ip -> client -> active) {
		if (ip -> client -> active -> expiry > cur_time) {
			note ("Trying recorded lease %s",
			      piaddr (ip -> client -> active -> address));
			/* Run the client script with the existing
			   parameters. */
			script_init (ip, "TIMEOUT",
				     ip -> client -> active -> medium);
			script_write_params (ip, "new_",
					     ip -> client -> active);
			if (ip -> client -> alias)
				script_write_params (ip, "alias_",
						     ip -> client -> alias);

			/* If the old lease is still good and doesn't
			   yet need renewal, go into BOUND state and
			   timeout at the renewal time. */
			if (!script_go (ip)) {
				if (cur_time <
				    ip -> client -> active -> renewal) {
					ip -> client -> state = S_BOUND;
					note ("bound: renewal in %d seconds.",
					      ip -> client -> active -> renewal
					      - cur_time);
					add_timeout ((ip -> client ->
						      active -> renewal),
						     state_bound, ip);
				} else {
					ip -> client -> state = S_BOUND;
					note ("bound: immediate renewal.");
					state_bound (ip);
				}
				reinitialize_interfaces ();
				go_daemon ();
				return;
			}
		}

		/* If there are no other leases, give up. */
		if (!ip -> client -> leases) {
			ip -> client -> leases = ip -> client -> active;
			ip -> client -> active = (struct client_lease *)0;
			break;
		}

	activate_next:
		/* Otherwise, put the active lease at the end of the
		   lease list, and try another lease.. */
		for (lp = ip -> client -> leases; lp -> next; lp = lp -> next)
			;
		lp -> next = ip -> client -> active;
		lp -> next -> next = (struct client_lease *)0;
		ip -> client -> active = ip -> client -> leases;
		ip -> client -> leases = ip -> client -> leases -> next;

		/* If we already tried this lease, we've exhausted the
		   set of leases, so we might as well give up for
		   now. */
		if (ip -> client -> active == loop)
			break;
	}

	/* No leases were available, or what was available didn't work, so
	   tell the shell script that we failed to allocate an address,
	   and try again later. */
	note ("No working leases in persistent database - sleeping.\n");
	script_init (ip, "FAIL", (struct string_list *)0);
	if (ip -> client -> alias)
		script_write_params (ip, "alias_", ip -> client -> alias);
	script_go (ip);
	ip -> client -> state = S_INIT;
	add_timeout (cur_time + ip -> client -> config -> retry_interval,
		     state_init, ip);
}

void send_request (ip)
	struct interface_info *ip;
{
	int result;
	int interval;
	struct sockaddr_in destination;
	struct in_addr from;

	/* Figure out how long it's been since we started transmitting. */
	interval = cur_time - ip -> client -> first_sending;

	/* If we're in INIT-REBOOT and we're past the reboot timeout,
	   go to INIT and see if we can DISCOVER an address... */
	/* XXX if we don't get an ACK, it means either that we're on
	   a network with no DHCP server, or that our server is down.
	   In the latter case, DHCPDISCOVER will get us a new address,
	   but we could also have successfully reused our old address.
	   In the former case, we're hosed anyway.   This is not a win-prone
	   situation. */
	if (ip -> client -> state == S_REBOOTING &&
	    interval > ip -> client -> config -> reboot_timeout) {
		ip -> client -> state = S_INIT;
		cancel_timeout (send_request, ip);
		state_init (ip);
		return;
	}

	/* If the lease has expired, relinquish the address and go back
	   to the INIT state. */
	if (ip -> client -> state != S_REQUESTING &&
	    cur_time > ip -> client -> active -> expiry) {
		/* Run the client script with the new parameters. */
		script_init (ip, "EXPIRE", (struct string_list *)0);
		script_write_params (ip, "old_", ip -> client -> active);
		if (ip -> client -> alias)
			script_write_params (ip, "alias_",
					     ip -> client -> alias);
		script_go (ip);

		ip -> client -> state = S_INIT;
		state_init (ip);
		return;
	}

	/* Do the exponential backoff... */
	if (!ip -> client -> interval)
		ip -> client -> interval = 1;
	else
		ip -> client -> interval +=
			random () % (2 * ip -> client -> interval);

	/* Don't backoff past 30 seconds. */
	if (ip -> client -> interval > 30)
		ip -> client -> interval =
			15 + random () % ip -> client -> interval;

	/* If the backoff would take us to the expiry time, just set the
	   timeout to the expiry time. */
	if (ip -> client -> state != S_REQUESTING &&
	    cur_time + ip -> client -> interval >
	    ip -> client -> active -> expiry)
		ip -> client -> interval =
			ip -> client -> active -> expiry - cur_time + 1;

	/* If the lease T2 time has elapsed, or if we're not yet bound,
	   broadcast the DHCPREQUEST rather than unicasting. */
	if (ip -> client -> state == S_REQUESTING ||
	    cur_time > ip -> client -> active -> rebind)
		destination.sin_addr.s_addr = INADDR_BROADCAST;
	else
		memcpy (&destination.sin_addr.s_addr,
			ip -> client -> destination.iabuf,
			sizeof destination.sin_addr.s_addr);
	destination.sin_port = remote_port;
	destination.sin_family = AF_INET;
#ifdef HAVE_SA_LEN
	destination.sin_len = sizeof destination;
#endif

	if (ip -> client -> state != S_REQUESTING)
		memcpy (&from, ip -> client -> active -> address.iabuf,
			sizeof from);
	else
		from.s_addr = INADDR_ANY;

	note ("DHCPREQUEST on %s to %s port %d", ip -> name,
	      inet_ntoa (destination.sin_addr),
	      ntohs (destination.sin_port));

#ifdef USE_FALLBACK
	if (destination.sin_addr.s_addr != INADDR_BROADCAST)
		result = send_fallback (&fallback_interface,
					(struct packet *)0,
					&ip -> client -> packet,
					ip -> client -> packet_length,
					from, &destination,
					(struct hardware *)0);
	else
#endif /* USE_FALLBACK */
		/* Send out a packet. */
		result = send_packet (ip, (struct packet *)0,
				      &ip -> client -> packet,
				      ip -> client -> packet_length,
				      from, &destination,
				      (struct hardware *)0);

	if (result < 0)
		warn ("send_packet: %m");

	add_timeout (cur_time + ip -> client -> interval,
		     send_request, ip);
}

void send_decline (ip)
	struct interface_info *ip;
{
	int result;

	note ("DHCPDECLINE on %s to %s port %d", ip -> name,
	      inet_ntoa (sockaddr_broadcast.sin_addr),
	      ntohs (sockaddr_broadcast.sin_port));

	/* Send out a packet. */
	result = send_packet (ip, (struct packet *)0,
			      &ip -> client -> packet,
			      ip -> client -> packet_length,
			      inaddr_any, &sockaddr_broadcast,
			      (struct hardware *)0);
	if (result < 0)
		warn ("send_packet: %m");
}

void send_release (ip)
	struct interface_info *ip;
{
	int result;

	note ("DHCPRELEASE on %s to %s port %d", ip -> name,
	      inet_ntoa (sockaddr_broadcast.sin_addr),
	      ntohs (sockaddr_broadcast.sin_port));

	/* Send out a packet. */
	result = send_packet (ip, (struct packet *)0,
			      &ip -> client -> packet,
			      ip -> client -> packet_length,
			      inaddr_any, &sockaddr_broadcast,
			      (struct hardware *)0);
	if (result < 0)
		warn ("send_packet: %m");
}

void make_discover (ip, lease)
	struct interface_info *ip;
	struct client_lease *lease;
{
	struct dhcp_packet *raw;
	unsigned char discover = DHCPDISCOVER;
	int i;

	struct tree_cache *options [256];
	struct tree_cache option_elements [256];

	memset (option_elements, 0, sizeof option_elements);
	memset (options, 0, sizeof options);
	memset (&ip -> client -> packet, 0, sizeof (ip -> client -> packet));

	/* Set DHCP_MESSAGE_TYPE to DHCPDISCOVER */
	i = DHO_DHCP_MESSAGE_TYPE;
	options [i] = &option_elements [i];
	options [i] -> value = &discover;
	options [i] -> len = sizeof discover;
	options [i] -> buf_size = sizeof discover;
	options [i] -> timeout = 0xFFFFFFFF;
	options [i] -> tree = (struct tree *)0;

	/* Request the options we want */
	i  = DHO_DHCP_PARAMETER_REQUEST_LIST;
	options [i] = &option_elements [i];
	options [i] -> value = ip -> client -> config -> requested_options;
	options [i] -> len = ip -> client -> config -> requested_option_count;
	options [i] -> buf_size =
		ip -> client -> config -> requested_option_count;
	options [i] -> timeout = 0xFFFFFFFF;
	options [i] -> tree = (struct tree *)0;

	/* If we had an address, try to get it again. */
	if (lease) {
		ip -> client -> requested_address = lease -> address;
		i = DHO_DHCP_REQUESTED_ADDRESS;
		options [i] = &option_elements [i];
		options [i] -> value = lease -> address.iabuf;
		options [i] -> len = lease -> address.len;
		options [i] -> buf_size = lease -> address.len;
		options [i] -> timeout = 0xFFFFFFFF;
		options [i] -> tree = (struct tree *)0;
	} else {
		ip -> client -> requested_address.len = 0;
	}

	/* Send any options requested in the config file. */
	for (i = 0; i < 256; i++) {
		if (!options [i] &&
		    ip -> client -> config -> send_options [i].data) {
			options [i] = &option_elements [i];
			options [i] -> value = ip -> client -> config ->
				send_options [i].data;
			options [i] -> len = ip -> client -> config ->
				send_options [i].len;
			options [i] -> buf_size = ip -> client -> config ->
				send_options [i].len;
			options [i] -> timeout = 0xFFFFFFFF;
			options [i] -> tree = (struct tree *)0;
		}
	}

	/* Set up the option buffer... */
	ip -> client -> packet_length =
		cons_options ((struct packet *)0, &ip -> client -> packet,
			      options, 0, 0);
	if (ip -> client -> packet_length < BOOTP_MIN_LEN)
		ip -> client -> packet_length = BOOTP_MIN_LEN;

	ip -> client -> packet.op = BOOTREQUEST;
	ip -> client -> packet.htype = ip -> hw_address.htype;
	ip -> client -> packet.hlen = ip -> hw_address.hlen;
	ip -> client -> packet.hops = 0;
	ip -> client -> packet.xid = random ();
	ip -> client -> packet.secs = 0; /* filled in by send_discover. */
	ip -> client -> packet.flags = htons (BOOTP_BROADCAST); /* XXX */
	memset (&(ip -> client -> packet.ciaddr),
		0, sizeof ip -> client -> packet.ciaddr);
	memset (&(ip -> client -> packet.yiaddr),
		0, sizeof ip -> client -> packet.yiaddr);
	memset (&(ip -> client -> packet.siaddr),
		0, sizeof ip -> client -> packet.siaddr);
	memset (&(ip -> client -> packet.giaddr),
		0, sizeof ip -> client -> packet.giaddr);
	memcpy (ip -> client -> packet.chaddr,
		ip -> hw_address.haddr, ip -> hw_address.hlen);

#ifdef DEBUG_PACKET
	dump_packet (sendpkt);
	dump_raw ((unsigned char *)ip -> client -> packet,
		  sendpkt->packet_length);
#endif
}


void make_request (ip, lease)
	struct interface_info *ip;
	struct client_lease *lease;
{
	unsigned char request = DHCPREQUEST;
	int i;

	struct tree_cache *options [256];
	struct tree_cache option_elements [256];

	memset (options, 0, sizeof options);
	memset (&ip -> client -> packet, 0, sizeof (ip -> client -> packet));

	/* Set DHCP_MESSAGE_TYPE to DHCPREQUEST */
	i = DHO_DHCP_MESSAGE_TYPE;
	options [i] = &option_elements [i];
	options [i] -> value = &request;
	options [i] -> len = sizeof request;
	options [i] -> buf_size = sizeof request;
	options [i] -> timeout = 0xFFFFFFFF;
	options [i] -> tree = (struct tree *)0;

	/* Request the options we want */
	i = DHO_DHCP_PARAMETER_REQUEST_LIST;
	options [i] = &option_elements [i];
	options [i] -> value = ip -> client -> config -> requested_options;
	options [i] -> len = ip -> client -> config -> requested_option_count;
	options [i] -> buf_size =
		ip -> client -> config -> requested_option_count;
	options [i] -> timeout = 0xFFFFFFFF;
	options [i] -> tree = (struct tree *)0;

	/* If we are requesting an address that hasn't yet been assigned
	   to us, use the DHCP Requested Address option. */
	if (ip -> client -> state == S_REQUESTING) {
		/* Send back the server identifier... */
		i = DHO_DHCP_SERVER_IDENTIFIER;
		options [i] = &option_elements [i];
		options [i] -> value = lease -> options [i].data;
		options [i] -> len = lease -> options [i].len;
		options [i] -> buf_size = lease -> options [i].len;
		options [i] -> timeout = 0xFFFFFFFF;
		options [i] -> tree = (struct tree *)0;
	}
	if (ip -> client -> state == S_REQUESTING ||
	    ip -> client -> state == S_REBOOTING) {
		ip -> client -> requested_address = lease -> address;
		i = DHO_DHCP_REQUESTED_ADDRESS;
		options [i] = &option_elements [i];
		options [i] -> value = lease -> address.iabuf;
		options [i] -> len = lease -> address.len;
		options [i] -> buf_size = lease -> address.len;
		options [i] -> timeout = 0xFFFFFFFF;
		options [i] -> tree = (struct tree *)0;
	} else {
		ip -> client -> requested_address.len = 0;
	}

	/* Send any options requested in the config file. */
	for (i = 0; i < 256; i++) {
		if (!options [i] &&
		    ip -> client -> config -> send_options [i].data) {
			options [i] = &option_elements [i];
			options [i] -> value = ip -> client -> config ->
				send_options [i].data;
			options [i] -> len = ip -> client -> config ->
				send_options [i].len;
			options [i] -> buf_size = ip -> client -> config ->
				send_options [i].len;
			options [i] -> timeout = 0xFFFFFFFF;
			options [i] -> tree = (struct tree *)0;
		}
	}

	/* Set up the option buffer... */
	ip -> client -> packet_length =
		cons_options ((struct packet *)0, &ip -> client -> packet,
			      options, 0, 0);
	if (ip -> client -> packet_length < BOOTP_MIN_LEN)
		ip -> client -> packet_length = BOOTP_MIN_LEN;

	ip -> client -> packet.op = BOOTREQUEST;
	ip -> client -> packet.htype = ip -> hw_address.htype;
	ip -> client -> packet.hlen = ip -> hw_address.hlen;
	ip -> client -> packet.hops = 0;
	ip -> client -> packet.xid = ip -> client -> xid;
	ip -> client -> packet.secs = 0; /* Filled in by send_request. */
	ip -> client -> packet.flags = htons (BOOTP_BROADCAST);

	/* If we own the address we're requesting, put it in ciaddr;
	   otherwise set ciaddr to zero. */
	if (ip -> client -> state == S_BOUND ||
	    ip -> client -> state == S_RENEWING ||
	    ip -> client -> state == S_REBINDING)
		memcpy (&ip -> client -> packet.ciaddr,
			lease -> address.iabuf, lease -> address.len);
	else
		memset (&ip -> client -> packet.ciaddr, 0,
			sizeof ip -> client -> packet.ciaddr);

	memset (&ip -> client -> packet.yiaddr, 0,
		sizeof ip -> client -> packet.yiaddr);
	memset (&ip -> client -> packet.siaddr, 0,
		sizeof ip -> client -> packet.siaddr);
	memset (&ip -> client -> packet.giaddr, 0,
		sizeof ip -> client -> packet.giaddr);
	memcpy (ip -> client -> packet.chaddr,
		ip -> hw_address.haddr, ip -> hw_address.hlen);

#ifdef DEBUG_PACKET
	dump_packet (sendpkt);
	dump_raw ((unsigned char *)ip -> client -> packet, sendpkt->packet_length);
#endif
}

void make_decline (ip, lease)
	struct interface_info *ip;
	struct client_lease *lease;
{
	unsigned char decline = DHCPDECLINE;
	int i;

	struct tree_cache *options [256];
	struct tree_cache message_type_tree;
	struct tree_cache requested_address_tree;
	struct tree_cache server_id_tree;

	memset (options, 0, sizeof options);
	memset (&ip -> client -> packet, 0, sizeof (ip -> client -> packet));

	/* Set DHCP_MESSAGE_TYPE to DHCPDECLINE */
	i = DHO_DHCP_MESSAGE_TYPE;
	options [i] = &message_type_tree;
	options [i] -> value = &decline;
	options [i] -> len = sizeof decline;
	options [i] -> buf_size = sizeof decline;
	options [i] -> timeout = 0xFFFFFFFF;
	options [i] -> tree = (struct tree *)0;

	/* Send back the server identifier... */
	i = DHO_DHCP_SERVER_IDENTIFIER;
        options [i] = &server_id_tree;
        options [i] -> value = lease -> options [i].data;
        options [i] -> len = lease -> options [i].len;
        options [i] -> buf_size = lease -> options [i].len;
        options [i] -> timeout = 0xFFFFFFFF;
        options [i] -> tree = (struct tree *)0;

	/* Send back the address we're declining. */
	i = DHO_DHCP_REQUESTED_ADDRESS;
	options [i] = &requested_address_tree;
	options [i] -> value = lease -> address.iabuf;
	options [i] -> len = lease -> address.len;
	options [i] -> buf_size = lease -> address.len;
	options [i] -> timeout = 0xFFFFFFFF;
	options [i] -> tree = (struct tree *)0;

	/* Set up the option buffer... */
	ip -> client -> packet_length =
		cons_options ((struct packet *)0, &ip -> client -> packet,
			      options, 0, 0);
	if (ip -> client -> packet_length < BOOTP_MIN_LEN)
		ip -> client -> packet_length = BOOTP_MIN_LEN;

	ip -> client -> packet.op = BOOTREQUEST;
	ip -> client -> packet.htype = ip -> hw_address.htype;
	ip -> client -> packet.hlen = ip -> hw_address.hlen;
	ip -> client -> packet.hops = 0;
	ip -> client -> packet.xid = ip -> client -> xid;
	ip -> client -> packet.secs = 0; /* Filled in by send_request. */
	ip -> client -> packet.flags = htons (BOOTP_BROADCAST);

	/* ciaddr must always be zero. */
	memset (&ip -> client -> packet.ciaddr, 0,
		sizeof ip -> client -> packet.ciaddr);
	memset (&ip -> client -> packet.yiaddr, 0,
		sizeof ip -> client -> packet.yiaddr);
	memset (&ip -> client -> packet.siaddr, 0,
		sizeof ip -> client -> packet.siaddr);
	memset (&ip -> client -> packet.giaddr, 0,
		sizeof ip -> client -> packet.giaddr);
	memcpy (ip -> client -> packet.chaddr,
		ip -> hw_address.haddr, ip -> hw_address.hlen);

#ifdef DEBUG_PACKET
	dump_packet (sendpkt);
	dump_raw ((unsigned char *)ip -> client -> packet, sendpkt->packet_length);
#endif
}

void make_release (ip, lease)
	struct interface_info *ip;
	struct client_lease *lease;
{
	unsigned char request = DHCPRELEASE;
	int i;

	struct tree_cache *options [256];
	struct tree_cache message_type_tree;
	struct tree_cache requested_address_tree;
	struct tree_cache server_id_tree;

	memset (options, 0, sizeof options);
	memset (&ip -> client -> packet, 0, sizeof (ip -> client -> packet));

	/* Set DHCP_MESSAGE_TYPE to DHCPRELEASE */
	i = DHO_DHCP_MESSAGE_TYPE;
	options [i] = &message_type_tree;
	options [i] -> value = &request;
	options [i] -> len = sizeof request;
	options [i] -> buf_size = sizeof request;
	options [i] -> timeout = 0xFFFFFFFF;
	options [i] -> tree = (struct tree *)0;

	/* Send back the server identifier... */
	i = DHO_DHCP_SERVER_IDENTIFIER;
        options [i] = &server_id_tree;
        options [i] -> value = lease -> options [i].data;
        options [i] -> len = lease -> options [i].len;
        options [i] -> buf_size = lease -> options [i].len;
        options [i] -> timeout = 0xFFFFFFFF;
        options [i] -> tree = (struct tree *)0;

	/* Set up the option buffer... */
	ip -> client -> packet_length =
		cons_options ((struct packet *)0, &ip -> client -> packet,
			      options, 0, 0);
	if (ip -> client -> packet_length < BOOTP_MIN_LEN)
		ip -> client -> packet_length = BOOTP_MIN_LEN;

	ip -> client -> packet.op = BOOTREQUEST;
	ip -> client -> packet.htype = ip -> hw_address.htype;
	ip -> client -> packet.hlen = ip -> hw_address.hlen;
	ip -> client -> packet.hops = 0;
	ip -> client -> packet.xid = ip -> client -> packet.xid;
	ip -> client -> packet.secs = 0;
	ip -> client -> packet.flags = 0;
	memcpy (&ip -> client -> packet.ciaddr,
		lease -> address.iabuf, lease -> address.len);
	memset (&ip -> client -> packet.yiaddr, 0,
		sizeof ip -> client -> packet.yiaddr);
	memset (&ip -> client -> packet.siaddr, 0,
		sizeof ip -> client -> packet.siaddr);
	memset (&ip -> client -> packet.giaddr, 0,
		sizeof ip -> client -> packet.giaddr);
	memcpy (ip -> client -> packet.chaddr,
		ip -> hw_address.haddr, ip -> hw_address.hlen);

#ifdef DEBUG_PACKET
	dump_packet (sendpkt);
	dump_raw ((unsigned char *)ip -> client -> packet,
		  ip -> client -> packet_length);
#endif
}

void free_client_lease (lease)
	struct client_lease *lease;
{
	int i;

	if (lease -> server_name)
		free (lease -> server_name);
	if (lease -> filename)
		free (lease -> filename);
	for (i = 0; i < 256; i++) {
		if (lease -> options [i].len)
			free (lease -> options [i].data);
	}
	free (lease);
}

FILE *leaseFile;

void rewrite_client_leases ()
{
	struct interface_info *ip;
	struct client_lease *lp;

	if (leaseFile)
		fclose (leaseFile);
	leaseFile = fopen (path_dhclient_db, "w");
	if (!leaseFile)
		error ("can't create /var/db/dhclient.leases: %m");

	/* Write out all the leases attached to configured interfaces that
	   we know about. */
	for (ip = interfaces; ip; ip = ip -> next) {
		for (lp = ip -> client -> leases; lp; lp = lp -> next) {
			write_client_lease (ip, lp);
		}
		if (ip -> client -> active)
			write_client_lease (ip, ip -> client -> active);
	}

	/* Write out any leases that are attached to interfaces that aren't
	   currently configured. */
	for (ip = dummy_interfaces; ip; ip = ip -> next) {
		for (lp = ip -> client -> leases; lp; lp = lp -> next) {
			write_client_lease (ip, lp);
		}
		if (ip -> client -> active)
			write_client_lease (ip, ip -> client -> active);
	}
	fflush (leaseFile);
}

void write_client_lease (ip, lease)
	struct interface_info *ip;
	struct client_lease *lease;
{
	int i;
	struct tm *t;

	/* If the lease came from the config file, we don't need to stash
	   a copy in the lease database. */
	if (lease -> is_static)
		return;

	if (!leaseFile) {	/* XXX */
		leaseFile = fopen (path_dhclient_db, "w");
		if (!leaseFile)
			error ("can't create /var/db/dhclient.leases: %m");
	}

	fprintf (leaseFile, "lease {\n");
	fprintf (leaseFile, "  interface \"%s\";\n", ip -> name);
	fprintf (leaseFile, "  fixed-address %s;\n",
		 piaddr (lease -> address));
	if (lease -> filename)
		fprintf (leaseFile, "  filename \"%s\";\n",
			 lease -> filename);
	if (lease -> server_name)
		fprintf (leaseFile, "  server-name \"%s\";\n",
			 lease -> filename);
	if (lease -> medium)
		fprintf (leaseFile, "  medium \"%s\";\n",
			 lease -> medium -> string);
	for (i = 0; i < 256; i++) {
		if (lease -> options [i].len) {
			fprintf (leaseFile,
				 "  option %s %s;\n",
				 dhcp_options [i].name,
				 pretty_print_option
				 (i, lease -> options [i].data,
				  lease -> options [i].len, 1));
		}
	}
	t = gmtime (&lease -> renewal);
	fprintf (leaseFile,
		 "  renew %d %d/%d/%d %02d:%02d:%02d;\n",
		 t -> tm_wday, t -> tm_year + 1900,
		 t -> tm_mon + 1, t -> tm_mday,
		 t -> tm_hour, t -> tm_min, t -> tm_sec);
	t = gmtime (&lease -> rebind);
	fprintf (leaseFile,
		 "  rebind %d %d/%d/%d %02d:%02d:%02d;\n",
		 t -> tm_wday, t -> tm_year + 1900,
		 t -> tm_mon + 1, t -> tm_mday,
		 t -> tm_hour, t -> tm_min, t -> tm_sec);
	t = gmtime (&lease -> expiry);
	fprintf (leaseFile,
		 "  expire %d %d/%d/%d %02d:%02d:%02d;\n",
		 t -> tm_wday, t -> tm_year + 1900,
		 t -> tm_mon + 1, t -> tm_mday,
		 t -> tm_hour, t -> tm_min, t -> tm_sec);
	fprintf (leaseFile, "}\n");
	fflush (leaseFile);
}

/* Variables holding name of script and file pointer for writing to
   script.   Needless to say, this is not reentrant - only one script
   can be invoked at a time. */
char scriptName [256];
FILE *scriptFile;

void script_init (ip, reason, medium)
	struct interface_info *ip;
	char *reason;
	struct string_list *medium;
{
	strcpy (scriptName, "/tmp/dcsXXXXXX");
	mktemp (scriptName);

	scriptFile = fopen (scriptName, "w");
	if (!scriptFile)
		error ("can't write script file: %m");
	fprintf (scriptFile, "#!/bin/sh\n\n");
	if (ip) {
		fprintf (scriptFile, "interface=\"%s\"\n", ip -> name);
		fprintf (scriptFile, "export interface\n");
	}
	if (medium) {
		fprintf (scriptFile, "medium=\"%s\"\n", medium -> string);
		fprintf (scriptFile, "export medium\n");
	}
	fprintf (scriptFile, "reason=\"%s\"\n", reason);
	fprintf (scriptFile, "export reason\n");
}

void script_write_params (ip, prefix, lease)
	struct interface_info *ip;
	char *prefix;
	struct client_lease *lease;
{
	int i;

	fprintf (scriptFile, "%sip_address=\"%s\"\n",
		 prefix, piaddr (lease -> address));
	fprintf (scriptFile, "export %sip_address\n", prefix);
	if (lease -> filename) {
		fprintf (scriptFile, "%sfilename=\"%s\";\n",
			 prefix, lease -> filename);
		fprintf (scriptFile, "export %sfilename\n", prefix);
	}
	if (lease -> server_name) {
		fprintf (scriptFile, "%sserver_name=\"%s\";\n",
			 prefix, lease -> server_name);
		fprintf (scriptFile, "export %sserver_name\n", prefix);
	}
	for (i = 0; i < 256; i++) {
		if (lease -> options [i].len) {
			char *s = dhcp_option_ev_name (&dhcp_options [i]);
			fprintf (scriptFile,
				 "%s%s=\"%s\"\n", prefix, s,
				 pretty_print_option
				 (i, lease -> options [i].data,
				  lease -> options [i].len, 0));
			fprintf (scriptFile, "export %s%s\n", prefix, s);
		}
	}
	fprintf (scriptFile, "%sexpiry=\"%d\"\n",
		 prefix, (int)lease -> expiry); /* XXX */
	fprintf (scriptFile, "export %sexpiry\n", prefix);
}

int script_go (ip)
	struct interface_info *ip;
{
	int rval;

	if (ip)
		fprintf (scriptFile, "%s\n",
			 ip -> client -> config -> script_name);
	else
		fprintf (scriptFile, "%s\n",
			 top_level_config.script_name);
	fprintf (scriptFile, "exit $?\n");
	fclose (scriptFile);
	chmod (scriptName, 0700);
	rval = system (scriptName);	
	/* unlink (scriptName); */
	return rval;
}

char *dhcp_option_ev_name (option)
	struct option *option;
{
	static char evbuf [256];
	int i;

	if (strlen (option -> name) + 1 > sizeof evbuf)
		error ("option %s name is larger than static buffer.");
	for (i = 0; option -> name [i]; i++) {
		if (option -> name [i] == '-')
			evbuf [i] = '_';
		else
			evbuf [i] = option -> name [i];
	}

	evbuf [i] = 0;
	return evbuf;
}

void go_daemon ()
{
	static int state = 0;
	int pid;

	/* Don't become a daemon if the user requested otherwise. */
	if (no_daemon)
		return;

	/* Only do it once. */
	if (state)
		return;
	state = 1;

	/* Stop logging to stderr... */
	log_perror = 0;

	/* Become a daemon... */
	if ((pid = fork ()) < 0)
		error ("Can't fork daemon: %m");
	else if (pid)
		exit (0);
	/* Become session leader and get pid... */
	pid = setsid ();
}

