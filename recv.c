// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2012-2019  B.A.T.M.A.N. contributors:
 *
 * Simon Wunderlich
 *
 * License-Filename: LICENSES/preferred/GPL-2.0
 */

#include <errno.h>
#include <net/ethernet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "alfred.h"
#include "batadv_query.h"
#include "hash.h"
#include "list.h"
#include "packet.h"

static int finish_alfred_push_data(struct globals *globals,
				   struct ether_addr mac,
				   struct alfred_push_data_v0 *push)
{
	unsigned int len, data_len;
	bool new_entry_created;
	struct alfred_data *data;
	struct dataset *dataset;
	uint8_t *pos;

	/* test already done in process_alfred_push_data */
	len = ntohs(push->header.length);
	if (len < sizeof(*push) - sizeof(push->header))
		return -1;

	len -= sizeof(*push) - sizeof(push->header);
	pos = (uint8_t *)push->data;

	while (len >= sizeof(*data)) {
		data = (struct alfred_data *)pos;
		data_len = ntohs(data->header.length);

		/* check if enough data is available */
		if (data_len + sizeof(*data) > len)
			break;

		new_entry_created = false;
		dataset = hash_find(globals->data_hash, data);
		if (!dataset) {
			dataset = malloc(sizeof(*dataset));
			if (!dataset)
				goto err;

			dataset->buf = NULL;
			dataset->data_source = SOURCE_SYNCED;

			memcpy(&dataset->data, data, sizeof(*data));
			if (hash_add(globals->data_hash, dataset)) {
				free(dataset);
				goto err;
			}
			new_entry_created = true;
		}
		/* don't overwrite our own data */
		if (dataset->data_source == SOURCE_LOCAL)
			goto skip_data;

		clock_gettime(CLOCK_MONOTONIC, &dataset->last_seen);

		/* check that data was changed */
		if (new_entry_created ||
		    dataset->data.header.length != data_len ||
		    memcmp(dataset->buf, data->data, data_len) != 0)
			changed_data_type(globals, data->header.type);

		/* free old buffer */
		if (dataset->buf) {
			free(dataset->buf);
			dataset->data.header.length = 0;
		}

		dataset->buf = malloc(data_len);

		/* that's not good */
		if (!dataset->buf)
			goto err;

		dataset->data.header.length = data_len;
		dataset->data.header.version = data->header.version;
		memcpy(dataset->buf, data->data, data_len);

		/* if the sender is also the the source of the dataset, we
		 * got a first hand dataset. */
		if (memcmp(&mac, data->source, ETH_ALEN) == 0)
			dataset->data_source = SOURCE_FIRST_HAND;
		else
			dataset->data_source = SOURCE_SYNCED;
skip_data:
		pos += (sizeof(*data) + data_len);
		len -= (sizeof(*data) + data_len);
	}
	return 0;
err:
	return -1;
}

struct transaction_head *
transaction_add(struct globals *globals, struct ether_addr mac, uint16_t id)
{
	struct transaction_head *head;

	head = malloc(sizeof(*head));
	if (!head)
		return NULL;

	head->server_addr = mac;
	head->id = id;
	head->requested_type = 0;
	head->txend_packets = 0;
	head->num_packet = 0;
	head->client_socket = -1;
	clock_gettime(CLOCK_MONOTONIC, &head->last_rx_time);
	INIT_LIST_HEAD(&head->packet_list);
	if (hash_add(globals->transaction_hash, head)) {
		free(head);
		return NULL;
	}

	return head;
}

struct transaction_head *transaction_clean(struct globals *globals,
					   struct transaction_head *head)
{
	struct transaction_packet *transaction_packet, *safe;

	list_for_each_entry_safe(transaction_packet, safe, &head->packet_list,
				 list) {
		list_del(&transaction_packet->list);
		free(transaction_packet->push);
		free(transaction_packet);
	}

	hash_remove(globals->transaction_hash, head);
	return head;
}

static int finish_alfred_transaction(struct globals *globals,
				     struct transaction_head *head,
				     struct ether_addr mac)
{
	struct transaction_packet *transaction_packet, *safe;

	/* finish when all packets received */
	if (!transaction_finished(head))
		return 0;

	list_for_each_entry_safe(transaction_packet, safe, &head->packet_list,
				 list) {
		finish_alfred_push_data(globals, mac, transaction_packet->push);

		list_del(&transaction_packet->list);
		free(transaction_packet->push);
		free(transaction_packet);
	}

	transaction_clean(globals, head);

	if (head->client_socket < 0)
		free(head);
	else
		unix_sock_req_data_finish(globals, head);

	return 1;
}

static int process_alfred_push_data(struct globals *globals,
				    struct interface *interface,
				    alfred_addr *source,
				    struct alfred_push_data_v0 *push)
{
	unsigned int len;
	struct ether_addr mac;
	int ret;
	struct transaction_head search, *head;
	struct transaction_packet *transaction_packet;
	int found;

	if (globals->ipv4mode)
		ret = ipv4_to_mac(interface, source, &mac);
	else
		ret = ipv6_to_mac(source, &mac);
	if (ret < 0)
		goto err;

	len = ntohs(push->header.length);
	if (len < sizeof(*push) - sizeof(push->header))
		goto err;

	search.server_addr = mac;
	search.id = ntohs(push->tx.id);

	head = hash_find(globals->transaction_hash, &search);
	if (!head) {
		/* slave must create the transactions to be able to correctly
		 *  wait for it */
		if (globals->opmode != OPMODE_MASTER)
			goto err;

		head = transaction_add(globals, mac, ntohs(push->tx.id));
		if (!head)
			goto err;
	}
	clock_gettime(CLOCK_MONOTONIC, &head->last_rx_time);

	found = 0;
	list_for_each_entry(transaction_packet, &head->packet_list, list) {
		if (transaction_packet->push->tx.seqno == push->tx.seqno) {
			found = 1;
			break;
		}
	}

	/* it seems the packet was duplicated */
	if (found)
		return 0;

	transaction_packet = malloc(sizeof(*transaction_packet));
	if (!transaction_packet)
		goto err;

	transaction_packet->push = malloc(len + sizeof(push->header));
	if (!transaction_packet->push) {
		free(transaction_packet);
		goto err;
	}

	memcpy(transaction_packet->push, push, len + sizeof(push->header));
	list_add_tail(&transaction_packet->list, &head->packet_list);
	head->num_packet++;

	finish_alfred_transaction(globals, head, mac);

	return 0;
err:
	return -1;
}

static int
process_alfred_announce_master(struct globals *globals,
			       struct interface *interface,
			       alfred_addr *source,
			       struct alfred_announce_master_v0 *announce)
{
	struct server *server;
	struct ether_addr mac;
	int ret;

	if (globals->ipv4mode)
		ret = ipv4_to_mac(interface, source, &mac);
	else
		ret = ipv6_to_mac(source, &mac);
	if (ret < 0)
		return -1;

	if (announce->header.version != ALFRED_VERSION)
		return -1;

	/* skip header.length check because "announce" has no extra fields */
	BUILD_BUG_ON(sizeof(*announce) - sizeof(announce->header) != 0);

	server = hash_find(interface->server_hash, &mac);
	if (!server) {
		server = malloc(sizeof(*server));
		if (!server)
			return -1;

		memcpy(&server->hwaddr, &mac, ETH_ALEN);
		memcpy(&server->address, source, sizeof(*source));
		server->tq = 0;

		if (hash_add(interface->server_hash, server)) {
			free(server);
			return -1;
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &server->last_seen);

	return 0;
}

static int process_alfred_request(struct globals *globals,
				  struct interface *interface,
				  alfred_addr *source,
				  struct alfred_request_v0 *request)
{
	unsigned int len;

	len = ntohs(request->header.length);

	if (request->header.version != ALFRED_VERSION)
		return -1;

	if (len < (sizeof(*request) - sizeof(request->header)))
		return -1;

	push_data(globals, interface, source, SOURCE_SYNCED,
		  request->requested_type, request->tx_id);

	return 0;
}

static int process_alfred_status_txend(struct globals *globals,
				       struct interface *interface,
				       alfred_addr *source,
				       struct alfred_status_v0 *request)
{
	struct transaction_head search, *head;
	struct ether_addr mac;
	unsigned int len;
	int ret;

	len = ntohs(request->header.length);

	if (request->header.version != ALFRED_VERSION)
		return -1;

	if (len < (sizeof(*request) - sizeof(request->header)))
		return -1;

	if (globals->ipv4mode)
		ret = ipv4_to_mac(interface, source, &mac);
	else
		ret = ipv6_to_mac(source, &mac);
	if (ret < 0)
		return -1;

	search.server_addr = mac;
	search.id = ntohs(request->tx.id);

	head = hash_find(globals->transaction_hash, &search);
	if (!head) {
		/* slave must create the transactions to be able to correctly
		 *  wait for it */
		if (globals->opmode != OPMODE_MASTER)
			goto err;

		/* 0-packet txend for unknown transaction */
		if (ntohs(request->tx.seqno) == 0)
			goto err;

		head = transaction_add(globals, mac, ntohs(request->tx.id));
		if (!head)
			goto err;
	}
	clock_gettime(CLOCK_MONOTONIC, &head->last_rx_time);

	head->txend_packets = ntohs(request->tx.seqno);
	finish_alfred_transaction(globals, head, mac);

	return 0;
err:
	return -1;
}

int recv_alfred_packet(struct globals *globals, struct interface *interface,
		       int recv_sock)
{
	uint8_t buf[MAX_PAYLOAD];
	ssize_t length;
	struct alfred_tlv *packet;
	struct sockaddr_in *source;
	struct sockaddr_in source4;
	struct sockaddr_in6 source6;
	socklen_t sourcelen;
	alfred_addr alfred_source;

	if (interface->netsock < 0)
		return -1;

	if (globals->ipv4mode) {
		source = (struct sockaddr_in *)&source4;
		sourcelen = sizeof(source4);
	} else {
		source = (struct sockaddr_in *)&source6;
		sourcelen = sizeof(source6);
	}

	length = recvfrom(recv_sock, buf, sizeof(buf), 0,
			  (struct sockaddr *)source, &sourcelen);
	if (length <= 0) {
		perror("read from network socket failed");
		return -1;
	}

	packet = (struct alfred_tlv *)buf;

	memset(&alfred_source, 0, sizeof(alfred_source));
	if (globals->ipv4mode) {
		memcpy(&alfred_source, &source4.sin_addr, sizeof(source4.sin_addr));
	} else {
		memcpy(&alfred_source, &source6.sin6_addr, sizeof(source6.sin6_addr));

		/* drop packets not sent over link-local ipv6 */
		if (!is_ipv6_eui64(&alfred_source.ipv6))
			return -1;
	}

	/* drop packets from ourselves */
	if (netsock_own_address(globals, &alfred_source))
		return -1;

	/* drop truncated packets */
	if (length < (int)sizeof(*packet) ||
	    length < (int)(ntohs(packet->length) + sizeof(*packet)))
		return -1;

	/* drop incompatible packet */
	if (packet->version != ALFRED_VERSION)
		return -1;

	switch (packet->type) {
	case ALFRED_PUSH_DATA:
		process_alfred_push_data(globals, interface, &alfred_source,
					 (struct alfred_push_data_v0 *)packet);
		break;
	case ALFRED_ANNOUNCE_MASTER:
		process_alfred_announce_master(globals, interface,
					       &alfred_source,
					       (struct alfred_announce_master_v0 *)packet);
		break;
	case ALFRED_REQUEST:
		process_alfred_request(globals, interface, &alfred_source,
				       (struct alfred_request_v0 *)packet);
		break;
	case ALFRED_STATUS_TXEND:
		process_alfred_status_txend(globals, interface, &alfred_source,
					    (struct alfred_status_v0 *)packet);
		break;
	default:
		/* unknown packet type */
		return -1;
	}

	return 0;
}
