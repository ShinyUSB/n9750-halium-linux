/* wda-setdf — WDA Set Data Format over QRTR via libqmi 1.32.2, reproducing
 * stock netmgrd's netmgr_wda_set_data_format for msmnile (rmnet_ipa0):
 *   link_layer = RAW_IP, ul/dl aggregation protocol = QMAPV4 (0x08),
 *   ul agg = 16384 bytes / 32 datagrams, dl agg = 16384 bytes / 31 datagrams.
 * (Stock also sets flow_control=1 and dl_minimum_padding=0; libqmi 1.32 has no
 *  input setters for those — omission equals modem defaults.)
 *
 * Prints the full granted response. Exit 0 iff the operation succeeds.
 */
#include <stdio.h>
#include <glib.h>
#include <libqmi-glib.h>
#include <libqrtr-glib.h>

static GMainLoop *loop;
static GError *err;
static QrtrBus *bus;
static QrtrNode *node;
static QmiDevice *device;
static QmiClientWda *client;

static void done(int rc)
{
	g_main_loop_quit(loop);
	if (rc)
		exit(rc);
}


static void set_df_ready(QmiClientWda *c, GAsyncResult *res)
{
	QmiMessageWdaSetDataFormatOutput *output;
	guint32 v32; guint8 v8;
	const gchar *str;

	output = qmi_client_wda_set_data_format_finish(c, res, &err);
	if (!output) {
		printf("WDA Set Data Format FAILED: %s\n", err ? err->message : "?");
		done(1);
		return;
	}
	if (!qmi_message_wda_set_data_format_output_get_result(output, &err)) {
		printf("WDA Set Data Format error: %s\n", err ? err->message : "?");
		qmi_message_wda_set_data_format_output_unref(output);
		done(1);
		return;
	}
	printf("WDA Set Data Format: SUCCESS\n");
	if (qmi_message_wda_set_data_format_output_get_link_layer_protocol(output, &v32, NULL))
		printf("  link_layer_protocol = %u\n", v32);
	if (qmi_message_wda_set_data_format_output_get_uplink_data_aggregation_protocol(output, &v32, NULL))
		printf("  ul_aggregation_protocol = %u\n", v32);
	if (qmi_message_wda_set_data_format_output_get_downlink_data_aggregation_protocol(output, &v32, NULL))
		printf("  dl_aggregation_protocol = %u\n", v32);
	if (qmi_message_wda_set_data_format_output_get_downlink_data_aggregation_max_size(output, &v32, NULL))
		printf("  dl_aggregation_max_size = %u\n", v32);
	if (qmi_message_wda_set_data_format_output_get_downlink_data_aggregation_max_datagrams(output, &v32, NULL))
		printf("  dl_aggregation_max_datagrams = %u\n", v32);
	if (qmi_message_wda_set_data_format_output_get_uplink_data_aggregation_max_size(output, &v32, NULL))
		printf("  ul_aggregation_max_size = %u\n", v32);
	if (qmi_message_wda_set_data_format_output_get_uplink_data_aggregation_max_datagrams(output, &v32, NULL))
		printf("  ul_aggregation_max_datagrams = %u\n", v32);
	if (qmi_message_wda_set_data_format_output_get_download_minimum_padding(output, &v32, NULL))
		printf("  dl_minimum_padding = %u\n", v32);
	if (qmi_message_wda_set_data_format_output_get_flow_control(output, &v8, NULL))
		printf("  flow_control = %u\n", v8);
	qmi_message_wda_set_data_format_output_unref(output);
	done(0);
}

static void alloc_ready(QmiDevice *d, GAsyncResult *res)
{
	QmiMessageWdaSetDataFormatInput *input;

	client = qmi_device_allocate_client_finish(d, res, &err);
	if (!client) {
		printf("WDA client alloc failed: %s\n", err ? err->message : "?");
		done(1);
		return;
	}
	printf("WDA client allocated, CID=%u\n", qmi_client_get_cid(QMI_CLIENT(client)));

	input = qmi_message_wda_set_data_format_input_new();
	qmi_message_wda_set_data_format_input_set_link_layer_protocol(
		input, QMI_WDA_LINK_LAYER_PROTOCOL_RAW_IP, NULL);
	qmi_message_wda_set_data_format_input_set_uplink_data_aggregation_protocol(
		input, QMI_WDA_DATA_AGGREGATION_PROTOCOL_QMAPV4, NULL);
	qmi_message_wda_set_data_format_input_set_downlink_data_aggregation_protocol(
		input, QMI_WDA_DATA_AGGREGATION_PROTOCOL_QMAPV4, NULL);
	qmi_message_wda_set_data_format_input_set_uplink_data_aggregation_max_size(
		input, 16384, NULL);
	qmi_message_wda_set_data_format_input_set_uplink_data_aggregation_max_datagrams(
		input, 32, NULL);
	qmi_message_wda_set_data_format_input_set_downlink_data_aggregation_max_size(
		input, 16384, NULL);
	qmi_message_wda_set_data_format_input_set_downlink_data_aggregation_max_datagrams(
		input, 31, NULL);
	qmi_message_wda_set_data_format_input_set_endpoint_info(
		input, QMI_DATA_ENDPOINT_TYPE_EMBEDDED, 1, NULL);

	qmi_client_wda_set_data_format(client, input, 15, NULL,
				       (GAsyncReadyCallback)set_df_ready, NULL);
	qmi_message_wda_set_data_format_input_unref(input);
}

static void open_ready(QmiDevice *d, GAsyncResult *res)
{
	if (!qmi_device_open_finish(d, res, &err)) {
		printf("device open failed: %s\n", err ? err->message : "?");
		done(1);
		return;
	}
	qmi_device_allocate_client(d, QMI_SERVICE_WDA, QMI_CID_NONE, 15, NULL,
				   (GAsyncReadyCallback)alloc_ready, NULL);
}

static void dev_ready(GObject *o, GAsyncResult *res)
{
	device = qmi_device_new_from_node_finish(res, &err);
	if (!device) {
		printf("device create failed: %s\n", err ? err->message : "?");
		done(1);
		return;
	}
	qmi_device_open(device, QMI_DEVICE_OPEN_FLAGS_NONE, 15, NULL,
			(GAsyncReadyCallback)open_ready, NULL);
}

static void bus_ready(GObject *s, GAsyncResult *res)
{
	bus = qrtr_bus_new_finish(res, &err);
	if (!bus) {
		printf("qrtr bus failed: %s\n", err ? err->message : "?");
		done(1);
		return;
	}
	node = qrtr_bus_get_node(bus, 0); /* qrtr://0 = modem */
	if (!node) {
		printf("qrtr node 0 not found\n");
		done(1);
		return;
	}
	qmi_device_new_from_node(node, NULL, (GAsyncReadyCallback)dev_ready, NULL);
}

int main(void)
{
	loop = g_main_loop_new(NULL, FALSE);
	qrtr_bus_new(1000, NULL, (GAsyncReadyCallback)bus_ready, NULL);
	g_main_loop_run(loop);
	return 0;
}
