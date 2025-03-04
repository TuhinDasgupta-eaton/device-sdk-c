/*
 * Copyright (c) 2020
 * IoTech Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */

#include "data-mqtt.h"
#include "correlation.h"
#include "service.h"
#include "iot/base64.h"
#include "iot/time.h"
#include <MQTTAsync.h>

#define DEFAULTPUBTOPIC "edgex/events/device"
#define DEFAULTREQTOPIC "edgex/device/command/request/%s/#"
#define DEFAULTRESPTOPIC "edgex/device/command/response"

void edgex_mqtt_config_defaults (iot_data_t *allconf, const char *svcname)
{
  char *reqt = malloc (sizeof (DEFAULTREQTOPIC) + strlen (svcname));
  sprintf (reqt, DEFAULTREQTOPIC, svcname);

  iot_data_string_map_add (allconf, EX_BUS_DISABLED, iot_data_alloc_bool (false));
  iot_data_string_map_add (allconf, EX_BUS_PROTOCOL, iot_data_alloc_string ("", IOT_DATA_REF));
  iot_data_string_map_add (allconf, EX_BUS_HOST, iot_data_alloc_string ("localhost", IOT_DATA_REF));
  iot_data_string_map_add (allconf, EX_BUS_PORT, iot_data_alloc_ui16 (0));
  iot_data_string_map_add (allconf, EX_BUS_TOPIC, iot_data_alloc_string (DEFAULTPUBTOPIC, IOT_DATA_REF));
  iot_data_string_map_add (allconf, EX_BUS_AUTHMODE, iot_data_alloc_string ("none", IOT_DATA_REF));
  iot_data_string_map_add (allconf, EX_BUS_SECRETNAME, iot_data_alloc_string ("", IOT_DATA_REF));

  iot_data_string_map_add (allconf, EX_BUS_CLIENTID, iot_data_alloc_string ("", IOT_DATA_REF));
  iot_data_string_map_add (allconf, EX_BUS_QOS, iot_data_alloc_ui16 (0));
  iot_data_string_map_add (allconf, EX_BUS_KEEPALIVE, iot_data_alloc_ui16 (60));
  iot_data_string_map_add (allconf, EX_BUS_RETAINED, iot_data_alloc_bool (false));
  iot_data_string_map_add (allconf, EX_BUS_CERTFILE, iot_data_alloc_string ("", IOT_DATA_REF));
  iot_data_string_map_add (allconf, EX_BUS_KEYFILE, iot_data_alloc_string ("", IOT_DATA_REF));
  iot_data_string_map_add (allconf, EX_BUS_SKIPVERIFY, iot_data_alloc_bool (false));

  iot_data_string_map_add (allconf, EX_BUS_TOPIC_CMDREQ, iot_data_alloc_string (reqt, IOT_DATA_TAKE));
  iot_data_string_map_add (allconf, EX_BUS_TOPIC_CMDRESP, iot_data_alloc_string (DEFAULTRESPTOPIC, IOT_DATA_REF));
}

JSON_Value *edgex_mqtt_config_json (const iot_data_t *allconf)
{
  JSON_Value *mqval = json_value_init_object ();
  JSON_Object *mqobj = json_value_get_object (mqval);
  json_object_set_string (mqobj, "Protocol", iot_data_string_map_get_string (allconf, EX_BUS_PROTOCOL));
  json_object_set_string (mqobj, "Host", iot_data_string_map_get_string (allconf, EX_BUS_HOST));
  json_object_set_uint (mqobj, "Port", iot_data_ui16 (iot_data_string_map_get (allconf, EX_BUS_PORT)));
  json_object_set_string (mqobj, "Topic", iot_data_string_map_get_string (allconf, EX_BUS_TOPIC));
  json_object_set_string (mqobj, "AuthMode", iot_data_string_map_get_string (allconf, EX_BUS_AUTHMODE));
  json_object_set_string (mqobj, "SecretName", iot_data_string_map_get_string (allconf, EX_BUS_SECRETNAME));

  JSON_Value *optval = json_value_init_object ();
  JSON_Object *optobj = json_value_get_object (optval);
  json_object_set_string (optobj, "ClientId", iot_data_string_map_get_string (allconf, EX_BUS_CLIENTID));
  json_object_set_number (optobj, "Qos", iot_data_ui16 (iot_data_string_map_get (allconf, EX_BUS_QOS)));
  json_object_set_number (optobj, "KeepAlive", iot_data_ui16 (iot_data_string_map_get (allconf, EX_BUS_KEEPALIVE)));
  json_object_set_boolean (optobj, "Retained", iot_data_bool (iot_data_string_map_get (allconf, EX_BUS_RETAINED)));
  json_object_set_string (optobj, "CertFile", iot_data_string_map_get_string (allconf, EX_BUS_CERTFILE));
  json_object_set_string (optobj, "KeyFile", iot_data_string_map_get_string (allconf, EX_BUS_KEYFILE));
  json_object_set_boolean (optobj, "SkipCertVerify", iot_data_bool (iot_data_string_map_get (allconf, EX_BUS_SKIPVERIFY)));
  json_object_set_value (mqobj, "Optional", optval);

  JSON_Value *topicval = json_value_init_object ();
  JSON_Object *topicobj = json_value_get_object (topicval);
  json_object_set_string (topicobj, "CommandRequestTopic", iot_data_string_map_get_string (allconf, EX_BUS_TOPIC_CMDREQ));
  json_object_set_string (topicobj, "CommandResponseTopicPrefix", iot_data_string_map_get_string (allconf, EX_BUS_TOPIC_CMDRESP));
  json_object_set_value (mqobj, "Topics", topicval);

  return mqval;
}

typedef struct edc_mqtt_conninfo
{
  MQTTAsync client;
  char *uri;
  iot_logger_t *lc;
  pthread_mutex_t mtx;
  pthread_cond_t cond;
  uint16_t qos;
  const char *topicbase;
  bool retained;
  bool connected;
} edc_mqtt_conninfo;

static void edc_mqtt_freefn (iot_logger_t *lc, void *address)
{
  edc_mqtt_conninfo *cinfo = (edc_mqtt_conninfo *)address;
  MQTTAsync_disconnectOptions opts = MQTTAsync_disconnectOptions_initializer;
  opts.context = cinfo->client;
  MQTTAsync_disconnect (cinfo->client, &opts);
  MQTTAsync_destroy (&cinfo->client);
  free (cinfo->uri);
  pthread_cond_destroy (&cinfo->cond);
  pthread_mutex_destroy (&cinfo->mtx);
  free (address);
}

static void edc_mqtt_onsend (void *context, MQTTAsync_successData *response)
{
  edc_mqtt_conninfo *cinfo = (edc_mqtt_conninfo *)context;
  iot_log_debug (cinfo->lc, "mqtt: published event");
}

static void edc_mqtt_onsendfail (void *context, MQTTAsync_failureData *response)
{
  edc_mqtt_conninfo *cinfo = (edc_mqtt_conninfo *)context;
  if (response->message)
  {
    iot_log_error (cinfo->lc, "mqtt: publish failed: %s (code %d)", response->message, response->code);
  }
  else
  {
    iot_log_error (cinfo->lc, "mqtt: publish failed, error code %d", response->code);
  }
}

static void edc_mqtt_postfn (iot_logger_t *lc, void *address, edgex_event_cooked *event)
{
  devsdk_http_reply h;
  int result;
  const char *crl;
  bool freecrl = false;
  size_t payloadsz;
  char *payload;
  char *json;
  char *topic;
  edc_mqtt_conninfo *cinfo = (edc_mqtt_conninfo *)address;
  MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
  MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;

  topic = malloc (strlen (cinfo->topicbase) + strlen (event->path) + 2);
  strcpy (topic, cinfo->topicbase);
  strcat (topic, "/");
  strcat (topic, event->path);

  edgex_event_cooked_write (event, &h);

  crl = edgex_device_get_crlid ();
  if (!crl)
  {
    edgex_device_alloc_crlid (NULL);
    freecrl = true;
    crl = edgex_device_get_crlid ();
  }

  payloadsz = iot_b64_encodesize (h.data.size);
  payload = malloc (payloadsz);
  iot_b64_encode (h.data.bytes, h.data.size, payload, payloadsz);

  JSON_Value *val = json_value_init_object ();
  JSON_Object *obj = json_value_get_object (val);
  json_object_set_string (obj, "Checksum", "");
  json_object_set_string (obj, "CorrelationID", crl);
  json_object_set_string (obj, "Payload", payload);
  json_object_set_string (obj, "ContentType", h.content_type);

  json = json_serialize_to_string (val);
  pubmsg.payload = json;
  pubmsg.payloadlen = strlen (json);
  pubmsg.qos = cinfo->qos;
  pubmsg.retained = cinfo->retained;
  opts.context = cinfo;
  opts.onSuccess = edc_mqtt_onsend;
  opts.onFailure = edc_mqtt_onsendfail;
  iot_log_trace (lc, "mqtt: publish event to topic %s", topic);
  result = MQTTAsync_sendMessage (cinfo->client, topic, &pubmsg, &opts);
  if (result != MQTTASYNC_SUCCESS)
  {
    iot_log_error (lc, "mqtt: failed to post event, error %d", result);
  }

  free (h.data.bytes);
  free (payload);
  json_free_serialized_string (json);
  free (topic);
  json_value_free (val);
  if (freecrl)
  {
    edgex_device_free_crlid ();
  }
}

static void edc_mqtt_onconnect (void *context, MQTTAsync_successData *response)
{
  edc_mqtt_conninfo *cinfo = (edc_mqtt_conninfo *)context;
  iot_log_info (cinfo->lc, "mqtt: connected");
  cinfo->connected = true;
  pthread_mutex_lock (&cinfo->mtx);
  pthread_cond_signal (&cinfo->cond);
  pthread_mutex_unlock (&cinfo->mtx);
}

static void edc_mqtt_onconnectfail (void *context, MQTTAsync_failureData *response)
{
  edc_mqtt_conninfo *cinfo = (edc_mqtt_conninfo *)context;
  if (response->message)
  {
    iot_log_error (cinfo->lc, "mqtt: connect failed: %s (code %d)", response->message, response->code);
  }
  else
  {
    iot_log_error (cinfo->lc, "mqtt: connect failed, error code %d", response->code);
  }
}

edgex_data_client_t *edgex_data_client_new_mqtt (devsdk_service_t *svc, const devsdk_timeout *tm, iot_threadpool_t *queue)
{
  int rc;
  char *uri;
  struct timespec max_wait;
  int timedout = 0;
  iot_data_t *secrets = NULL;
  iot_logger_t *lc = svc->logger;
  const iot_data_t *allconf = svc->config.sdkconf;
  MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;
  MQTTAsync_SSLOptions ssl_opts = MQTTAsync_SSLOptions_initializer;
  MQTTAsync_createOptions create_opts = MQTTAsync_createOptions_initializer;

  edgex_data_client_t *result = malloc (sizeof (edgex_data_client_t));
  edc_mqtt_conninfo *cinfo = calloc (1, sizeof (edc_mqtt_conninfo));

  const char *host = iot_data_string_map_get_string (allconf, EX_BUS_HOST);
  const char *prot = iot_data_string_map_get_string (allconf, EX_BUS_PROTOCOL);
  const char *certfile = iot_data_string_map_get_string (allconf, EX_BUS_CERTFILE);
  const char *keyfile = iot_data_string_map_get_string (allconf, EX_BUS_KEYFILE);
  uint16_t port = iot_data_ui16 (iot_data_string_map_get (allconf, EX_BUS_PORT));
  if (*prot == '\0')
  {
    prot = "tcp";
  }
  if (port == 0)
  {
    if (strcmp (prot, "ssl") == 0)
    {
      port = 8883;
    }
    else
    {
      port = 1883;
    }
  }

  uri = malloc (strlen (host) + strlen (prot) + 10);
  sprintf (uri, "%s://%s:%" PRIu16, prot, host, port);
  iot_log_info (lc, "Event data will be sent through MQTT at %s", uri);

  cinfo->lc = lc;
  result->lc = lc;
  result->queue = queue;
  result->pf = edc_mqtt_postfn;
  result->ff = edc_mqtt_freefn;
  result->mf = NULL;
  result->address = cinfo;

  cinfo->qos = iot_data_ui16 (iot_data_string_map_get (allconf, EX_BUS_QOS));
  cinfo->retained = iot_data_bool (iot_data_string_map_get (allconf, EX_BUS_RETAINED));
  cinfo->topicbase = iot_data_string_map_get_string (allconf, EX_BUS_TOPIC);
  cinfo->uri = uri;

  create_opts.sendWhileDisconnected = 1;
  rc = MQTTAsync_createWithOptions
    (&cinfo->client, uri, iot_data_string_map_get_string (allconf, EX_BUS_CLIENTID), MQTTCLIENT_PERSISTENCE_NONE, NULL, &create_opts);
  if (rc != MQTTASYNC_SUCCESS)
  {
    iot_log_error (lc, "mqtt: failed to create client, return code %d", rc);
    free (cinfo);
    free (result);
    free (uri);
    return NULL;
  }
  conn_opts.keepAliveInterval = iot_data_ui16 (iot_data_string_map_get (allconf, EX_BUS_KEEPALIVE));
  conn_opts.cleansession = 1;
  conn_opts.automaticReconnect = 1;
  conn_opts.onSuccess = edc_mqtt_onconnect;
  conn_opts.onFailure = edc_mqtt_onconnectfail;
  conn_opts.context = cinfo;
  if (strcmp (iot_data_string_map_get_string (allconf, EX_BUS_AUTHMODE), "usernamepassword") == 0)
  {
    secrets = devsdk_get_secrets (svc, iot_data_string_map_get_string (allconf, EX_BUS_SECRETNAME));
    conn_opts.username = iot_data_string_map_get_string (secrets, "username");
    conn_opts.password = iot_data_string_map_get_string (secrets, "password");
  }
  conn_opts.ssl = &ssl_opts;
  if (strlen (certfile))
  {
    ssl_opts.trustStore = certfile;
  }
  if (strlen (keyfile))
  {
    ssl_opts.keyStore = keyfile;
  }
  ssl_opts.verify = iot_data_string_map_get_bool (allconf, EX_BUS_SKIPVERIFY, false) ? 0 : 1;

  pthread_mutex_init (&cinfo->mtx, NULL);
  pthread_cond_init (&cinfo->cond, NULL);
  while (true)
  {
    uint64_t t1, t2;

    t1 = iot_time_msecs ();

    if (tm->deadline <= t1)
    {
      break;
    }
    max_wait.tv_sec = tm->deadline / 1000;
    max_wait.tv_nsec = 1000000 * (tm->deadline % 1000);

    pthread_mutex_lock (&cinfo->mtx);
    if ((rc = MQTTAsync_connect (cinfo->client, &conn_opts)) == MQTTASYNC_SUCCESS)
    {
      timedout = pthread_cond_timedwait (&cinfo->cond, &cinfo->mtx, &max_wait);
    }
    pthread_mutex_unlock (&cinfo->mtx);
    if (cinfo->connected)
    {
      break;
    }
    else
    {
      if (timedout == ETIMEDOUT)
      {
        iot_log_error (lc, "mqtt: failed to connect, timed out");
      }
      else
      {
        iot_log_error (lc, "mqtt: failed to connect, return code %d", rc);
      }
      t2 = iot_time_msecs ();
      if (t2 > tm->deadline - tm->interval)
      {
        break;
      }
      if (tm->interval > t2 - t1)
      {
        iot_wait_msecs (tm->interval - (t2 - t1));
      }
    }
  }
  iot_data_free (secrets);
  pthread_cond_destroy (&cinfo->cond);
  pthread_mutex_destroy (&cinfo->mtx);
  if (!cinfo->connected)
  {
    free (cinfo);
    free (result);
    free (uri);
    result = NULL;
  }
  return result;
}
