// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2012-2019  B.A.T.M.A.N. contributors:
 *
 * Simon Wunderlich
 *
 * License-Filename: LICENSES/preferred/GPL-2.0
 */

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include "alfred.h"
#include "hash.h"
#include "packet.h"
#include "list.h"

int announce_master(struct globals *globals)
{
	struct alfred_announce_master_v0 announcement;
	struct interface *interface;

	list_for_each_entry(interface, &globals->interfaces, list) {
		announcement.header.type = ALFRED_ANNOUNCE_MASTER;
		announcement.header.version = ALFRED_VERSION;
		announcement.header.length = htons(0);

		send_alfred_packet(globals, interface, &alfred_mcast,
				   &announcement, sizeof(announcement));
	}

	return 0;
}

int push_data(struct globals *globals, struct interface *interface,
	      alfred_addr *destination, enum data_source max_source_level,
	      int type_filter, uint16_t tx_id)
{
	struct hash_it_t *hashit = NULL;
	uint8_t buf[MAX_PAYLOAD];
	struct alfred_push_data_v0 *push;
	struct alfred_data *data;
	uint16_t total_length = 0;
	size_t tlv_length;
	uint16_t seqno = 0;
	uint16_t length;
	struct alfred_status_v0 status_end;

	push = (struct alfred_push_data_v0 *)buf;
	push->header.type = ALFRED_PUSH_DATA;
	push->header.version = ALFRED_VERSION;
	push->tx.id = tx_id;

	while (NULL != (hashit = hash_iterate(globals->data_hash, hashit))) {
		struct dataset *dataset = hashit->bucket->data;

		if (dataset->data_source > max_source_level)
			continue;

		if (type_filter >= 0 &&
		    dataset->data.header.type != type_filter)
			continue;

		/* would the packet be too big? send so far aggregated data
		 * first */
		if (total_length + dataset->data.header.length + sizeof(*data) >
		    MAX_PAYLOAD - sizeof(*push)) {
			/* is there any data to send? */
			if (total_length == 0)
				continue;

			tlv_length = total_length;
			tlv_length += sizeof(*push) - sizeof(push->header);
			push->header.length = htons(tlv_length);
			push->tx.seqno = htons(seqno++);
			send_alfred_packet(globals, interface, destination, push,
					   sizeof(*push) + total_length);
			total_length = 0;
		}

		/* still too large? - should never happen */
		if (total_length + dataset->data.header.length + sizeof(*data) >
		    MAX_PAYLOAD - sizeof(*push))
			continue;

		data = (struct alfred_data *)
		       (buf + sizeof(*push) + total_length);
		memcpy(data, &dataset->data, sizeof(*data));
		data->header.length = htons(data->header.length);
		memcpy(data->data, dataset->buf, dataset->data.header.length);

		total_length += dataset->data.header.length + sizeof(*data);
	}
	/* send the final packet */
	if (total_length) {
		tlv_length = total_length;
		tlv_length += sizeof(*push) - sizeof(push->header);
		push->header.length = htons(tlv_length);
		push->tx.seqno = htons(seqno++);
		send_alfred_packet(globals, interface, destination, push,
				   sizeof(*push) + total_length);
	}

	/* send transaction txend packet */
	if (seqno > 0 || type_filter != NO_FILTER) {
		status_end.header.type = ALFRED_STATUS_TXEND;
		status_end.header.version = ALFRED_VERSION;
		length = sizeof(status_end) - sizeof(status_end.header);
		status_end.header.length = htons(length);

		status_end.tx.id = tx_id;
		status_end.tx.seqno = htons(seqno);

		send_alfred_packet(globals, interface, destination, &status_end,
				   sizeof(status_end));
	}

	return 0;
}

int sync_data(struct globals *globals)
{
	struct hash_it_t *hashit = NULL;
	struct interface *interface;

	/* send local data and data from our clients to (all) other servers */
	list_for_each_entry(interface, &globals->interfaces, list) {
		while (NULL != (hashit = hash_iterate(interface->server_hash,
						      hashit))) {
			struct server *server = hashit->bucket->data;

			push_data(globals, interface, &server->address,
				  SOURCE_FIRST_HAND, NO_FILTER,
				  get_random_id());
		}
	}
	return 0;
}

int push_local_data(struct globals *globals)
{
	struct interface *interface;

	/* no server - yet */
	if (!globals->best_server)
		return -1;

	list_for_each_entry(interface, &globals->interfaces, list) {
		push_data(globals, interface, &globals->best_server->address,
			  SOURCE_LOCAL, NO_FILTER, get_random_id());
	}

	return 0;
}

ssize_t send_alfred_packet(struct globals *globals, struct interface *interface,
			   const alfred_addr *dest, void *buf, int length)
{
	ssize_t ret;
	struct sockaddr *dest_addr;
	struct sockaddr_in6 dest_addr6;
	struct sockaddr_in dest_addr4;
	socklen_t slen;

	if (globals->ipv4mode) {
		memset(&dest_addr4, 0, sizeof(dest_addr4));
		dest_addr4.sin_family = AF_INET;
		dest_addr4.sin_port = htons(ALFRED_PORT);
		memcpy(&dest_addr4.sin_addr, &dest->ipv4, sizeof(dest->ipv4));

		slen = sizeof(struct sockaddr_in);
		dest_addr = (struct sockaddr *)&dest_addr4;
	} else {
		memset(&dest_addr6, 0, sizeof(dest_addr6));
		dest_addr6.sin6_family = AF_INET6;
		dest_addr6.sin6_port = htons(ALFRED_PORT);
		dest_addr6.sin6_scope_id = interface->scope_id;
		memcpy(&dest_addr6.sin6_addr, &dest->ipv6, sizeof(dest->ipv6));

		slen = sizeof(struct sockaddr_in6);
		dest_addr = (struct sockaddr *)&dest_addr6;
	}

	if (interface->netsock < 0)
		return 0;

	ret = sendto(interface->netsock, buf, length, 0, dest_addr, slen);
	if (ret == -EPERM) {
		perror("Error during sent");
		close(interface->netsock);
		close(interface->netsock_mcast);
		interface->netsock = -1;
		interface->netsock_mcast = -1;
	}

	return ret;
}
