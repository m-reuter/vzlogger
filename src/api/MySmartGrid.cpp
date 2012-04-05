/***********************************************************************/
/** @file MySmartGrid.cpp
 * Header file for volkszaehler.org API calls
 *
 *  @author Kai Krueger
 *  @date   2012-03-15
 *  @email  kai.krueger@itwm.fraunhofer.de
 * @copyright Copyright (c) 2011, The volkszaehler.org project
 * @package vzlogger
 * @license http://opensource.org/licenses/gpl-license.php GNU Public License
 *
 * (C) Fraunhofer ITWM
 **/
/*---------------------------------------------------------------------*/

/*
 * This file is part of volkzaehler.org
 *
 * volkzaehler.org is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * volkzaehler.org is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with volkszaehler.org. If not, see <http://www.gnu.org/licenses/>.
 */


#include <openssl/ssl.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/evp.h>

#include <VZException.hpp>
#include "include/config.h"
#include <api/MySmartGrid.hpp>
#include <api/CurlCallback.hpp>

extern Config_Options options;

vz::api::MySmartGrid::MySmartGrid(
  Channel::Ptr ch,
  std::list<Option> pOptions
) 
    : ApiIF(ch)
    , _channelType(chn_type_device)
		, _scaler(1)
    , _response(new vz::api::CurlResponse())
    , _first_ts(0)
    , _first_counter(0)
    , _last_counter(0)

{
  OptionList optlist;
  print(log_debug, "===> Create MySmartGrid-API", channel()->name());
	char url[255];

  /* parse required options */
  try {
    _middleware  = optlist.lookup_string(pOptions, "middleware");
    _secretKey   = optlist.lookup_string(pOptions, "secretKey");

    std::string channelType = optlist.lookup_string(pOptions, "type");
    if(channelType == "device") _channelType = chn_type_device;
    else if(channelType == "sensor") _channelType = chn_type_sensor;
    else throw vz::VZException("Bad value for channel type.");

  } catch ( vz::OptionNotFoundException &e ) {
    throw;
  } catch ( vz::VZException &e ) {
    throw;
  }
  /* parse optional options */
  try {
    _interval   = optlist.lookup_int(pOptions, "interval");
  } catch ( vz::OptionNotFoundException &e ) {
    _interval = 300;  // default time between 2 logmessages
  } catch ( vz::VZException &e ) {
    throw;
  }
  try {
    _scaler   = optlist.lookup_int(pOptions, "scaler");
  } catch ( vz::OptionNotFoundException &e ) {
    _scaler   = 1;  // default scaling faktor
  } catch ( vz::VZException &e ) {
    throw;
  }

  convertUuid(channel()->uuid());
  
  switch(_channelType) {
      case chn_type_device:
        sprintf(url, "%s/device/%s", middleware().c_str(), uuid());			/* build url */
        break;
      case chn_type_sensor:
        sprintf(url, "%s/sensor/%s", middleware().c_str(), uuid());			/* build url */
        break;
  }
  
  print(log_debug, "msg_api_init() %s", channel()->name(), url);

  _api_header();
  
  curl_easy_setopt(_curlIF.handle(), CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(_curlIF.handle(), CURLOPT_SSL_VERIFYHOST, 0L);
  
	curl_easy_setopt(_curlIF.handle(), CURLOPT_URL, url);

  // CurlCallback::write_callback requires CurlResponse* as data
  curl_easy_setopt(_curlIF.handle(), CURLOPT_WRITEFUNCTION, &(vz::api::CurlCallback::write_callback));
  curl_easy_setopt(_curlIF.handle(), CURLOPT_WRITEDATA, response());

	curl_easy_setopt(_curlIF.handle(), CURLOPT_VERBOSE, options.verbosity());

  // CurlCallback::debug_callback requires CurlResponse* as data
	curl_easy_setopt(_curlIF.handle(), CURLOPT_DEBUGFUNCTION, &(vz::api::CurlCallback::debug_callback));
	curl_easy_setopt(_curlIF.handle(), CURLOPT_DEBUGDATA, response());
}

vz::api::MySmartGrid::~MySmartGrid() 
{
}

void vz::api::MySmartGrid::send() 
{
  json_object *json_obj;
  char digest[255];

  const char *json_str;
  long int http_code;
  CURLcode curl_code;
  
  // check if we want to send
  time_t now = time(NULL);
  
  if(_first_ts>0) {
    if ( (now-first_ts()) < interval() ) {
      print(log_debug, "api-MySmartGrid, skip message.", "");
      return;
    }
  } else { // _first_ts = 0
  }

  switch(_channelType) {
      case chn_type_device:
        json_obj = _apiDevice(channel()->buffer());
        break;
      case chn_type_sensor:
        json_obj = _apiSensor(channel()->buffer());
        break;
  }
  json_str = json_object_to_json_string(json_obj);

  print(log_debug, "JSON request body: %s", channel()->name(), json_str);

  /* initialize response */
  _response->clear_response();

  curl_easy_setopt(_curlIF.handle(), CURLOPT_POSTFIELDS, json_str);

  _api_header();
  hmac_sha1(digest, (const unsigned char*)json_str, strlen(json_str));
  _curlIF.addHeader(digest);
  print(log_debug, "Header_Digest: %s", channel()->name(), digest);

  _curlIF.commitHeader();

  curl_code = _curlIF.perform();
  curl_easy_getinfo(_curlIF.handle(), CURLINFO_RESPONSE_CODE, &http_code);

  /* check response */
  if (curl_code == CURLE_OK && http_code == 200) { /* everything is ok */
    print(log_debug, "Request succeeded with code: %i", channel()->name(), http_code);
  }
  else { /* error */
    channel()->buffer()->undelete();
    if (curl_code != CURLE_OK) {
      print(log_error, "CURL: %s", channel()->name(), curl_easy_strerror(curl_code));
    }
    else if (http_code != 200) {
      // 502 - Bad gateway
      char err[255];
      api_parse_exception(err, 255);
      print(log_error, "Error from middleware: %s", channel()->name(), err);
    }
  }

  /* householding */
  json_object_put(json_obj);

  if (options.daemon() && (curl_code != CURLE_OK || http_code != 200)) {
    print(log_info, "Waiting %i secs for next request due to previous failure",
          channel()->name(), options.retry_pause());
    sleep(options.retry_pause());
  }
  //sleep(20);
}

void vz::api::MySmartGrid::api_parse_exception(char *err, size_t n) {
	struct json_tokener *json_tok;
	struct json_object *json_obj_in;

	json_tok = json_tokener_new();
	json_obj_in = json_tokener_parse_ex(json_tok, _response->get_response().c_str(),
                                      _response->get_response().size());

	if (json_tok->err == json_tokener_success) {
		struct json_object *json_obj = json_object_object_get(json_obj_in, "exception");

		if (json_obj) {
			snprintf(err, n, "%s: %s",
				json_object_get_string(json_object_object_get(json_obj,  "type")),
				json_object_get_string(json_object_object_get(json_obj,  "message"))
			);
		} else {
      json_obj = json_object_object_get(json_obj_in, "response");
      if (json_obj) {
        snprintf(err, n, "Server-Error: %s", json_object_get_string(json_obj));
      } else {
        print(log_debug, "CURL - Resp: '%s'", channel()->name(), _response->get_response().c_str());
			strncpy(err, "missing exception", n);
      }
    }
	}
	else {
		strncpy(err, json_tokener_errors[json_tok->err], n);
	}

	json_object_put(json_obj_in);
	json_tokener_free(json_tok);
}

json_object *vz::api::MySmartGrid::_apiDevice(Buffer::Ptr buf) {
  
  if(_first_ts>0) { // send lifesign
    _first_ts = time(NULL);
    return _json_object_heartbeat(buf);
  } else{ // send  device registration
    _first_ts = time(NULL);
    return _json_object_registration(buf);
  }
}

json_object *vz::api::MySmartGrid::_apiSensor(Buffer::Ptr buf) {

  return _json_object_measurements(buf);
}


/*---------------------------------------------------------------------*/
/** 
 * @brief MySmartGrid Device Registration

 @param[in] buf   

 @return json-object
**/
/*---------------------------------------------------------------------*/
json_object * vz::api::MySmartGrid::_json_object_registration(Buffer::Ptr buf) {
  // url https://api.mysmartgrid.de:8443/device/<device id>
  // key : <device-id>
	json_object *json_obj    = json_object_new_object();
  
  json_object *jstring = json_object_new_string(secretKey());
  json_object_object_add(json_obj, "key", jstring);

  return json_obj;
}

/*---------------------------------------------------------------------*/
/** 
 * @brief MySmartGrid heartbeat message

 @param[in] buf   

 @return <ReturnValue>
**/
/*---------------------------------------------------------------------*/
json_object * vz::api::MySmartGrid::_json_object_heartbeat(Buffer::Ptr buf) {
  // url https://api.mysmartgrid.de:8443/device/<device id>
  //  memtotal:   <total RAM>,
  //  version:    <firmware version>, 
  //  memcached:  <total cache memory in MB>,
  //  membuffers: <total buffer memory in MB>,
  //  memfree:    <total free memory in MB>,
  //  uptime:     <total time the device is up and running>, 
  //  reset:      <number of times the device has been reseted>
  json_object *json_obj    = json_object_new_object();

  json_object_object_add(json_obj, "memtotal", json_object_new_string(""));
  json_object_object_add(json_obj, "version", json_object_new_string(""));
  json_object_object_add(json_obj, "memcached", json_object_new_string(""));
  json_object_object_add(json_obj, "membuffers", json_object_new_string(""));
  json_object_object_add(json_obj, "memfree", json_object_new_string(""));
  json_object_object_add(json_obj, "uptime", json_object_new_string(""));
  json_object_object_add(json_obj, "reset", json_object_new_string(""));

  return json_obj;
}

/*---------------------------------------------------------------------*/
/** 
 * @brief MySmartGrid event message

 @param[in] buf   

 @return <ReturnValue>
**/
/*---------------------------------------------------------------------*/
json_object * vz::api::MySmartGrid::_json_object_event(Buffer::Ptr buf) {
  // url https://api.mysmartgrid.de:8443/event/<event id>
  // device: <device id>
  json_object *json_obj    = json_object_new_object();

  json_object_object_add(json_obj, "device", json_object_new_string(channel()->uuid()));

  return json_obj;
}

/*---------------------------------------------------------------------*/
/** 
 * @brief MySmartGrid sensor configuration

 @param[in] buf   

 @return <ReturnValue>
**/
/*---------------------------------------------------------------------*/
json_object * vz::api::MySmartGrid::_json_object_sensor(Buffer::Ptr buf) {
  // url https://api.mysmartgrid.de:8443/sensor/<sensor id>
  // device:    <device id>,
  // function:  <sensor name>
  json_object *json_obj    = json_object_new_object();

  json_object_object_add(json_obj, "device", json_object_new_string(channel()->uuid()));
  json_object_object_add(json_obj, "function", json_object_new_string(""));

  return json_obj;
}

/*---------------------------------------------------------------------*/
/** 
 * @brief MySmartGrid sensor measurements message

 @param[in] buf   

 @return <ReturnValue>
**/
/*---------------------------------------------------------------------*/
json_object * vz::api::MySmartGrid::_json_object_measurements(Buffer::Ptr buf) {
  //  measurements: [[<timestamp1>,<value1>], [<timestamp2>,<value2>], ... ,[<timestamp n>,<value n>]]
	json_object *json_obj    = json_object_new_object();
	json_object *json_tuples = json_object_new_array();
	Buffer::iterator it;

  //long last_counter = 0;

  //  json_object_array_add(json_tuples, json_object_new_string("measurements"));
	for (it = buf->begin(); it != buf->end(); it++) {
		struct json_object *json_tuple = json_object_new_array();

		buf->lock();

		// TODO use long int of new json-c version
		// API requires milliseconds => * 1000
		long timestamp = it->tvtod();
		long value = it->value() * _scaler;
    it->mark_delete();
    
		buf->unlock();
    if( _first_counter < 1 ) {
      _first_counter = value;
      _last_counter = value;
    } else {
      if ( (_last_counter < value)  && (_first_ts < timestamp)) {
        _first_ts = timestamp;
        json_object_array_add(json_tuple, json_object_new_int(timestamp));
        json_object_array_add(json_tuple, json_object_new_int(value-_first_counter));
        json_object_array_add(json_tuples, json_tuple);
        _last_counter = value;
      }
    }
  }
  
  json_object_object_add(json_obj, "measurements", json_tuples);

	return json_obj;
}

void vz::api::MySmartGrid::_api_header() {
	char agent[255];

	/* prepare header */
	sprintf(agent, "User-Agent: %s/%s (%s)", PACKAGE, VERSION, curl_version());	/* build user agent */
  
  _curlIF.clearHeader();
  
	_curlIF.addHeader("Content-type: application/json");
	//_curlIF.addHeader("Accept: application/json");
	_curlIF.addHeader(agent);
	_curlIF.addHeader("X-Version: 1.0");

}

void vz::api::MySmartGrid::hmac_sha1(
  char *digest
  , const unsigned char *data
  ,size_t dataLen
  ) {
  HMAC_CTX hmacContext;

  HMAC_Init(&hmacContext, secretKey(), strlen(secretKey()), EVP_sha1());
  HMAC_Update(&hmacContext, data, dataLen);

  unsigned char out[EVP_MAX_MD_SIZE];
  unsigned int len = EVP_MAX_MD_SIZE;
  HMAC_Final(&hmacContext, out, &len);
  char ret[2*EVP_MAX_MD_SIZE];
  memset(ret, 0, sizeof(ret));
  
  for(int i=0; i<len; i++) {
    char s[4];
    snprintf(s, 3, "%02x:", out[i]);
    strncat(ret, s, 2*len);
    //strncat(ret, s, sizeof(ret));
  }
  snprintf(digest, 255/*sizeof(digest)*/, "X-Digest: %s", ret);
}


void vz::api::MySmartGrid::convertUuid(const std::string uuid) {
  std::stringstream oss;
  for(int i = 0; i < uuid.length(); i++) {
    if( uuid[i] != '-' )
      oss<< uuid[i];
  }
  _uuid = oss.str();
}
