/*
 * Simple RAK Field Tester Daemon for ChirpStack v3/v4
 */


/*******************************************************************************
 * Copyright (c) 2012, 2022 IBM Corp., and others
 * Copyright (c) 2023 Dana H. Myers
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v2.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *   https://www.eclipse.org/legal/epl-2.0/
 * and the Eclipse Distribution License is available at
 *   http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    Ian Craggs - initial contribution
 *******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "MQTTAsync.h"
#include "cJSON.h"
#include <math.h>

#include <pthread.h>
#include <unistd.h>

#define QOS         0

int disc_finished = 0;
int subscribed = 0;
int finished = 0;

void onConnect(void *context, MQTTAsync_successData * response);
void onConnectFailure(void *context, MQTTAsync_failureData * response);

/*
 *
 *
 */

/*
 * From:
 * https://nachtimwald.com/2017/11/18/base64-encode-and-decode-in-c/
 */

const char b64chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_encoded_size(size_t inlen)
{
	size_t ret;

	ret = inlen;
	if (inlen % 3 != 0)
		ret += 3 - (inlen % 3);
	ret /= 3;
	ret *= 4;

	return ret;
}

static char *b64_encode(const uint8_t * in, size_t len)
{
	char *out;
	size_t elen;
	size_t i;
	size_t j;
	size_t v;

	if (in == NULL || len == 0)
		return NULL;

	elen = b64_encoded_size(len);
	out = malloc(elen + 1);
	out[elen] = '\0';

	for (i = 0, j = 0; i < len; i += 3, j += 4) {
		v = in[i];
		v = i + 1 < len ? v << 8 | in[i + 1] : v << 8;
		v = i + 2 < len ? v << 8 | in[i + 2] : v << 8;

		out[j] = b64chars[(v >> 18) & 0x3F];
		out[j + 1] = b64chars[(v >> 12) & 0x3F];
		if (i + 1 < len) {
			out[j + 2] = b64chars[(v >> 6) & 0x3F];
		} else {
			out[j + 2] = '=';
		}
		if (i + 2 < len) {
			out[j + 3] = b64chars[v & 0x3F];
		} else {
			out[j + 3] = '=';
		}
	}

	return out;
}

/*
 * RFT uplink processing
 */
static double distance(double lat1, double lon1, double lat2, double lon2)
{
	double radlat1 = M_PI * lat1 / 180.0;
	double radlat2 = M_PI * lat2 / 180.0;
	double theta = lon1 - lon2;
	double radtheta = M_PI * theta / 180.0;
	double dist = sin(radlat1) * sin(radlat2) +
	    cos(radlat1) * cos(radlat2) * cos(radtheta);

	if (dist > 1.0) {
		dist = 1.0;
	}
	// XXX: collapse this
	dist = acos(dist);
	dist = dist * 180.0 / M_PI;
	dist = dist * 60.0 * 1.1515;
	dist = dist * 1.609344;
	return (dist);
}

/*
 *
 */
void processUplink(cJSON * report, char **topic, char **msg)
{
	bool isCSv4;
	char *str;
	char *dlTopic;
	cJSON *devEUI;
	cJSON *appName;
	cJSON *appId;
	cJSON *dlObject;
	cJSON *deviceInfo;
	cJSON *payload;
	cJSON *tmpObj;
	cJSON *gwObj, *rxInfo;
	cJSON *confirmed, *fPort, *data;
	double nodeLat, nodeLon;
	double minSNR = 100.0, maxSNR = -100.0;
	double minRSSI = 1000.0, maxRSSI = -1000.0;
	double minDistance = 100000.0, maxDistance = 0.0;
	uint8_t dl_buf[6];
	uint32_t gwCnt;

	*msg = *topic = NULL;

	// determine if CSv3 or CSv4: "deviceInfo" indicates v4
	isCSv4 = cJSON_IsObject(deviceInfo =
				cJSON_GetObjectItemCaseSensitive(report,
								 "deviceInfo"));

	// filter for "Mapper-App"
	appName =
	    cJSON_GetObjectItemCaseSensitive(isCSv4 ? deviceInfo : report,
					     "applicationName");

	// strcmp() here should be safe from overflow
	if (!cJSON_IsString(appName) ||
	    strcmp(appName->valuestring, "Mapper-App")) {
		return;
	}
	// pull up the RFT payload
	if (!cJSON_IsObject((payload =
			     cJSON_GetObjectItemCaseSensitive(report,
							      "object")))) {
		return;
	}

	if (isCSv4) {
		appId =
		    cJSON_GetObjectItemCaseSensitive(deviceInfo,
						     "applicationId");
		devEUI = cJSON_GetObjectItemCaseSensitive(deviceInfo, "devEui");
	} else {
		appId =
		    cJSON_GetObjectItemCaseSensitive(report, "applicationID");
		devEUI = cJSON_GetObjectItemCaseSensitive(report, "devEUI");
	}

	if (!cJSON_IsString(appId) || (appId->valuestring == NULL) ||
	    !cJSON_IsString(devEUI) || (devEUI->valuestring == NULL)) {
		return;
	}
	// check for presence of 'sats', 'hdop', 
	if (!cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(payload, "sats"))) {
		return;
	}

	if (!cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(payload, "hdop"))) {
		return;
	}
	// get latitude, longitude
	if (cJSON_IsNumber(tmpObj =
			   cJSON_GetObjectItemCaseSensitive(payload,
							    "latitude"))) {
		nodeLat = cJSON_GetNumberValue(tmpObj);
	} else {
		// not a valid payload
		return;
	}

	if (cJSON_IsNumber(tmpObj =
			   cJSON_GetObjectItemCaseSensitive(payload,
							    "longitude"))) {
		nodeLon = cJSON_GetNumberValue(tmpObj);
	} else {
		// not a valid payload
		return;
	}

	gwCnt = 0;
	rxInfo = cJSON_GetObjectItemCaseSensitive(report, "rxInfo");
	cJSON_ArrayForEach(gwObj, rxInfo) {
		cJSON *location =
		    cJSON_GetObjectItemCaseSensitive(gwObj, "location");
		cJSON *gwLat =
		    cJSON_GetObjectItemCaseSensitive(location, "latitude");
		cJSON *gwLon =
		    cJSON_GetObjectItemCaseSensitive(location, "longitude");
		cJSON *rssi = cJSON_GetObjectItemCaseSensitive(gwObj, "rssi");
		cJSON *snr = isCSv4 ? cJSON_GetObjectItemCaseSensitive(gwObj,
								       "snr") :
		    cJSON_GetObjectItemCaseSensitive(gwObj, "loRaSNR");
		double d, r, s;

		if (!cJSON_IsNumber(gwLat) || !cJSON_IsNumber(gwLon) ||
		    !cJSON_IsNumber(rssi) || !cJSON_IsNumber(snr)) {
			continue;
		}

		r = cJSON_GetNumberValue(rssi);
		s = cJSON_GetNumberValue(snr);
		d = 1000.0 * distance(nodeLat, nodeLon,
				      cJSON_GetNumberValue(gwLat),
				      cJSON_GetNumberValue(gwLon));

		if (s < minSNR) {
			minSNR = s;
		}

		if (s > maxSNR) {
			maxSNR = s;
		}

		if (r < minRSSI) {
			minRSSI = r;
		}

		if (r > maxRSSI) {
			maxRSSI = r;
		}

		if (d < minDistance) {
			minDistance = d;
		}

		if (d > maxDistance) {
			maxDistance = d;
		}

		gwCnt++;
	}

	dlTopic = malloc(1 +
			 snprintf(dl_buf, 0,
				  "application/%s/device/%s/command/down",
				  appId->valuestring, devEUI->valuestring));

	if (dlTopic == NULL) {
		return;
	}
	// special case: dlTopic will never overflow
	sprintf(dlTopic, "application/%s/device/%s/command/down",
		appId->valuestring, devEUI->valuestring);

	dl_buf[0] = 1;
	dl_buf[1] = (uint8_t) (minRSSI + 200);
	dl_buf[2] = (uint8_t) (maxRSSI + 200);
	dl_buf[3] = (uint8_t) (minDistance / 250.0);
	dl_buf[4] = (uint8_t) (maxDistance / 250.0);
	dl_buf[5] = (uint8_t) (gwCnt > 255 ? 255 : gwCnt);

	str = b64_encode(dl_buf, sizeof(dl_buf));

	dlObject = cJSON_CreateObject();
	if (cJSON_AddNumberToObject(dlObject, "fPort", 2) &&
	    cJSON_AddBoolToObject(dlObject, "confirmed", 0) &&
	    cJSON_AddStringToObject(dlObject, "devEui", devEUI->valuestring) &&
	    cJSON_AddStringToObject(dlObject, "data", str)) {
		// send
		char *s = cJSON_PrintUnformatted(dlObject);

		*topic = dlTopic;
		*msg = s;
	}

	free(str);
	cJSON_Delete(dlObject);
	return;
}

/*
 *
 */
void connlost(void *context, char *cause)
{
	MQTTAsync client = (MQTTAsync) context;
	MQTTAsync_connectOptions conn_opts =
	    MQTTAsync_connectOptions_initializer;
	int rc;

	conn_opts.keepAliveInterval = 20;
	conn_opts.cleansession = 1;
	conn_opts.onSuccess = onConnect;
	conn_opts.onFailure = onConnectFailure;
	if ((rc = MQTTAsync_connect(client, &conn_opts)) != MQTTASYNC_SUCCESS) {
		finished = 1;
	}
}

void onDisconnectFailure(void *context, MQTTAsync_failureData * response)
{
	finished = 1;
}

void onDisconnect(void *context, MQTTAsync_successData * response)
{
	finished = 1;
}

void onSendFailure(void *context, MQTTAsync_failureData * response)
{
	MQTTAsync client = (MQTTAsync) context;
	MQTTAsync_disconnectOptions opts =
	    MQTTAsync_disconnectOptions_initializer;
	int rc;

	opts.onSuccess = onDisconnect;
	opts.onFailure = onDisconnectFailure;
	opts.context = client;
	if ((rc = MQTTAsync_disconnect(client, &opts)) != MQTTASYNC_SUCCESS) {
		exit(EXIT_FAILURE);
	}
}

void onSend(void *context, MQTTAsync_successData * response)
{
	// XXX: no action required
}

int
msgarrvd(void *context, char *topicName, int topicLen,
	 MQTTAsync_message * message)
{
	cJSON *report;
	char *topic, *payload;
	MQTTAsync client = (MQTTAsync) context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	int rc;

	report = cJSON_ParseWithLength((char *)message->payload,
				       message->payloadlen);

	if (!report) {
		return (1);
	}

	processUplink(report, &topic, &payload);
	cJSON_Delete(report);

	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);

	if (topic) {
		// publish a response

		opts.onSuccess = onSend;
		opts.onFailure = onSendFailure;
		opts.context = client;
		pubmsg.payload = payload;
		pubmsg.payloadlen = (int)strlen(payload);
		pubmsg.qos = QOS;
		pubmsg.retained = 0;
		if ((rc =
		     MQTTAsync_sendMessage(client, topic, &pubmsg,
					   &opts)) != MQTTASYNC_SUCCESS) {
			free(topic);
			free(payload);
			exit(EXIT_FAILURE);
		}
		free(topic);
		free(payload);
	}

	return 1;
}

void onSubscribe(void *context, MQTTAsync_successData * response)
{
	subscribed = 1;
}

void onSubscribeFailure(void *context, MQTTAsync_failureData * response)
{
	finished = 1;
}

void onConnectFailure(void *context, MQTTAsync_failureData * response)
{
	finished = 1;
}

void onConnect(void *context, MQTTAsync_successData * response)
{
	MQTTAsync client = (MQTTAsync) context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;

	opts.onSuccess = onSubscribe;
	opts.onFailure = onSubscribeFailure;
	opts.context = client;
	if ((rc =
	     MQTTAsync_subscribe(client, "application/+/device/+/event/up",
				 QOS, &opts)) != MQTTASYNC_SUCCESS) {
		finished = 1;
	}
}

int main(int argc, char *argv[])
{
	MQTTAsync client;
	MQTTAsync_connectOptions conn_opts =
	    MQTTAsync_connectOptions_initializer;
	MQTTAsync_disconnectOptions disc_opts =
	    MQTTAsync_disconnectOptions_initializer;
	int rc, ch;

	if ((rc =
	     MQTTAsync_create(&client, "tcp://localhost:1883", "RAK-FT-Daemon",
			      MQTTCLIENT_PERSISTENCE_NONE,
			      NULL)) != MQTTASYNC_SUCCESS) {
		printf("rftd: failed to create client: %d\n", rc);
		rc = EXIT_FAILURE;
		goto exit;
	}

	if ((rc = MQTTAsync_setCallbacks(client, client, connlost,
					 msgarrvd,
					 NULL)) != MQTTASYNC_SUCCESS) {
		printf("rftd: failed to set callbacks: %d\n", rc);
		rc = EXIT_FAILURE;
		goto destroy_exit;
	}

	conn_opts.keepAliveInterval = 20;
	conn_opts.cleansession = 1;
	conn_opts.onSuccess = onConnect;
	conn_opts.onFailure = onConnectFailure;
	conn_opts.context = client;
	if ((rc = MQTTAsync_connect(client, &conn_opts)) != MQTTASYNC_SUCCESS) {
		printf("rftd: failed to start connect: %d\n", rc);
		rc = EXIT_FAILURE;
		goto destroy_exit;
	}

	while (!subscribed && !finished) {
		usleep(10000L);
	}

	if (finished) {
		goto exit;
	}
	// XXX: Need to learn a better trick than this
	{
		pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
		pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

		pthread_mutex_lock(&mutex);
		pthread_cond_wait(&cond, &mutex);
	}

#if 0
	disc_opts.onSuccess = onDisconnect;
	disc_opts.onFailure = onDisconnectFailure;
	if ((rc =
	     MQTTAsync_disconnect(client, &disc_opts)) != MQTTASYNC_SUCCESS) {
		printf("Failed to start disconnect, return code %d\n", rc);
		rc = EXIT_FAILURE;
		goto destroy_exit;
	}

	while (!disc_finished) {
		usleep(10000L);
	}

#endif
 destroy_exit:
	MQTTAsync_destroy(&client);
 exit:
	return rc;
}
