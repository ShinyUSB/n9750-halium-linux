/* wds-call — persistent WDS data-call helper for the d2q (reproduces stock
 * netmgrd's per-call flow with a retained client): allocate WDS client once,
 * bind mux data port (mux 1, embedded, iface 1), start network (IPv4),
 * print current settings, then HOLD the call open (the CID/packet-data handle
 * die if the QRTR socket closes). Ctrl-C / SIGTERM stops cleanly.
 */
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <glib.h>
#include <libqmi-glib.h>
#include <libqrtr-glib.h>

static GMainLoop *loop;
static GError *err;
static QmiDevice *device;
static QmiClientWds *client;
static guint32 packet_data_handle;

static void die(const char *m)
{
	printf("FAIL: %s: %s\n", m, err ? err->message : "?");
	g_main_loop_quit(loop);
}

static void stats_ready(QmiClientWds *c, GAsyncResult *res)
{
	QmiMessageWdsGetPacketStatisticsOutput *out;
	guint32 txp=0, rxp=0, txe=0, rxe=0;
	guint64 txb=0, rxb=0;
	out = qmi_client_wds_get_packet_statistics_finish(c, res, &err);
	if (!out || !qmi_message_wds_get_packet_statistics_output_get_result(out, &err)) {
		printf("stats: failed: %s\n", err ? err->message : "?");
		goto out;
	}
	qmi_message_wds_get_packet_statistics_output_get_tx_packets_ok(out, &txp, NULL);
	qmi_message_wds_get_packet_statistics_output_get_rx_packets_ok(out, &rxp, NULL);
	qmi_message_wds_get_packet_statistics_output_get_tx_packets_error(out, &txe, NULL);
	qmi_message_wds_get_packet_statistics_output_get_rx_packets_error(out, &rxe, NULL);
	qmi_message_wds_get_packet_statistics_output_get_tx_bytes_ok(out, &txb, NULL);
	qmi_message_wds_get_packet_statistics_output_get_rx_bytes_ok(out, &rxb, NULL);
	printf("STATS: tx_ok=%u rx_ok=%u tx_err=%u rx_err=%u tx_bytes=%llu rx_bytes=%llu\n",
	       txp, rxp, txe, rxe, (unsigned long long)txb, (unsigned long long)rxb);
out:
	if (out) qmi_message_wds_get_packet_statistics_output_unref(out);
}

static gboolean query_stats(gpointer ud)
{
	QmiMessageWdsGetPacketStatisticsInput *in;
	in = qmi_message_wds_get_packet_statistics_input_new();
	qmi_message_wds_get_packet_statistics_input_set_mask(in, 0xffffffff, NULL);
	qmi_client_wds_get_packet_statistics(client, in, 5, NULL, (GAsyncReadyCallback)stats_ready, NULL);
	qmi_message_wds_get_packet_statistics_input_unref(in);
	return G_SOURCE_CONTINUE;
}

static void get_settings_ready(QmiClientWds *c, GAsyncResult *res)
{
	QmiMessageWdsGetCurrentSettingsOutput *out;
	/* (unused) */
	guint32 v32;
	/* (unused) */

	out = qmi_client_wds_get_current_settings_finish(c, res, &err);
	if (!out || !qmi_message_wds_get_current_settings_output_get_result(out, &err)) {
		printf("get_current_settings failed: %s\n", err ? err->message : "?");
		goto out;
	}
	printf("=== current settings ===\n");
	if (qmi_message_wds_get_current_settings_output_get_ipv4_address(out, &v32, NULL)) {
		struct in_addr a; a.s_addr = g_htonl(v32);
		printf("  ipv4 address: %s\n", inet_ntoa(a));
	}
	if (qmi_message_wds_get_current_settings_output_get_ipv4_gateway_address(out, &v32, NULL)) {
		struct in_addr a; a.s_addr = g_htonl(v32);
		printf("  ipv4 gateway: %s\n", inet_ntoa(a));
	}
	if (qmi_message_wds_get_current_settings_output_get_mtu(out, &v32, NULL))
		printf("  mtu: %u\n", v32);
out:
	if (out) qmi_message_wds_get_current_settings_output_unref(out);
	printf("=== call is UP; holding (Ctrl-C to stop) ===\n");
	/* poll the modem's bearer packet stats every 3 s: reveals whether the
	 * modem receives our UL (rx_ok) and whether it sends DL (tx_ok) */
	g_timeout_add_seconds(3, query_stats, NULL);
}

static void start_ready(QmiClientWds *c, GAsyncResult *res)
{
	QmiMessageWdsStartNetworkOutput *out;

	out = qmi_client_wds_start_network_finish(c, res, &err);
	if (!out) { die("start_network send"); return; }
	if (!qmi_message_wds_start_network_output_get_result(out, &err)) {
		printf("start_network error: %s\n", err ? err->message : "?");
		qmi_message_wds_start_network_output_unref(out);
		g_main_loop_quit(loop);
		return;
	}
	if (qmi_message_wds_start_network_output_get_packet_data_handle(out, &packet_data_handle, NULL))
		printf("start_network SUCCESS, packet_data_handle=%u\n", packet_data_handle);
	else
		printf("start_network SUCCESS (no handle)\n");
	qmi_message_wds_start_network_output_unref(out);

	{
		QmiMessageWdsGetCurrentSettingsInput *in;
		in = qmi_message_wds_get_current_settings_input_new();
		qmi_message_wds_get_current_settings_input_set_requested_settings(
			in, (QmiWdsRequestedSettings)0x2310, NULL); /* dns|ip|gateway|mtu */
		qmi_client_wds_get_current_settings(c, in, 10, NULL,
						    (GAsyncReadyCallback)get_settings_ready, NULL);
		qmi_message_wds_get_current_settings_input_unref(in);
	}
}

static void bind_ready(QmiClientWds *c, GAsyncResult *res)
{
	QmiMessageWdsBindMuxDataPortOutput *out;
	QmiMessageWdsStartNetworkInput *input;

	out = qmi_client_wds_bind_mux_data_port_finish(c, res, &err);
	if (!out || !qmi_message_wds_bind_mux_data_port_output_get_result(out, &err)) {
		printf("bind FAILED: %s\n", err ? err->message : "?");
		if (out) qmi_message_wds_bind_mux_data_port_output_unref(out);
		g_main_loop_quit(loop);
		return;
	}
	printf("bind SUCCESS\n");
	qmi_message_wds_bind_mux_data_port_output_unref(out);

	input = qmi_message_wds_start_network_input_new();
	qmi_message_wds_start_network_input_set_ip_family_preference(
		input, QMI_WDS_IP_FAMILY_IPV4, NULL);
	qmi_client_wds_start_network(client, input, 30, NULL,
				     (GAsyncReadyCallback)start_ready, NULL);
	qmi_message_wds_start_network_input_unref(input);
}

static void alloc_ready(QmiDevice *d, GAsyncResult *res)
{
	QmiMessageWdsBindMuxDataPortInput *input;

	client = qmi_device_allocate_client_finish(d, res, &err);
	if (!client) { die("wds client alloc"); return; }
	printf("WDS client CID=%u\n", qmi_client_get_cid(QMI_CLIENT(client)));

	input = qmi_message_wds_bind_mux_data_port_input_new();
	qmi_message_wds_bind_mux_data_port_input_set_mux_id(input, 1, NULL);
	qmi_message_wds_bind_mux_data_port_input_set_endpoint_info(
		input, QMI_DATA_ENDPOINT_TYPE_EMBEDDED, 1, NULL);
	qmi_client_wds_bind_mux_data_port(client, input, 15, NULL,
					  (GAsyncReadyCallback)bind_ready, NULL);
	qmi_message_wds_bind_mux_data_port_input_unref(input);
}

static void open_ready(QmiDevice *d, GAsyncResult *res)
{
	if (!qmi_device_open_finish(d, res, &err)) { die("device open"); return; }
	qmi_device_allocate_client(d, QMI_SERVICE_WDS, QMI_CID_NONE, 15, NULL,
				   (GAsyncReadyCallback)alloc_ready, NULL);
}

static void dev_ready(GObject *o, GAsyncResult *res)
{
	device = qmi_device_new_from_node_finish(res, &err);
	if (!device) { die("device create"); return; }
	qmi_device_open(device, QMI_DEVICE_OPEN_FLAGS_NONE, 15, NULL,
			(GAsyncReadyCallback)open_ready, NULL);
}

static void bus_ready(GObject *s, GAsyncResult *res)
{
	QrtrBus *bus = qrtr_bus_new_finish(res, &err);
	QrtrNode *node;
	if (!bus) { die("qrtr bus"); return; }
	node = qrtr_bus_get_node(bus, 0);
	if (!node) { die("qrtr node 0"); return; }
	qmi_device_new_from_node(node, NULL, (GAsyncReadyCallback)dev_ready, NULL);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	loop = g_main_loop_new(NULL, FALSE);
	qrtr_bus_new(1000, NULL, (GAsyncReadyCallback)bus_ready, NULL);
	g_main_loop_run(loop);
	return 0;
}
