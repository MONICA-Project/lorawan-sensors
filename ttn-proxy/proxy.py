#!/usr/bin/env python3
import argparse
import base64
import json
import logging
import requests
import ttn

from datetime import datetime
from time import sleep


# change this to reduce logging output, e.g. use logging.INFO
logging.basicConfig(level=logging.DEBUG)

# some global variables
today = "{0:%Y%m%d}".format(datetime.now())
f_logging = today + "_proxy.log"
scale = 10.0;
auth_header = {}
datastreams = {}


def uplink_callback(msg, client):
    logging.debug("Uplink message")
    logging.debug("  FROM: %s", msg.dev_id)
    logging.debug("  TIME: %s", msg.metadata.time)
    logging.debug("   RAW: %s", msg.payload_raw)
    with open(f_logging, 'a') as fl:
        print(str(msg), file = fl)
    buf = base64.decodebytes(msg.payload_raw.encode('ascii'))
    # parse values from buffer
    humidity    = buf[0]
    temperature = ((0x0f & buf[2]) << 8)  |  (0xff & buf[1])
    windspeed   = ((0xff & buf[3]) << 4)  | ((0xf0 & buf[2]) >> 4)
    devid       = ((0xff & buf[7]) << 24) | ((0xff & buf[6]) << 16) | ((0xff & buf[5]) << 8) | (0xff & buf[4] << 0)
    # offset +500 and scale temperature
    temperature = round((temperature - 500) / scale, 2)
    # convert to m/s and scale windspeed
    windspeed   = round((windspeed / 3.600) / scale, 2)

    logging.info("  DATA: (%s, %s, %s, %s)", devid, humidity, temperature, windspeed)

    if msg.dev_id in datastreams:
        data = {}
        data['temperature'] = { 'result': temperature }
        data['humidity']    = { 'result': humidity }
        data['windspeed']   = { 'result': windspeed }
        for sensor in datastreams[msg.dev_id]:
            if windspeed > 200.0 and sensor == 'windspeed':
                logging.debug("  INVALID windspeed (%s), skipping ...", str(windspeed))
                continue
            url = datastreams[msg.dev_id][sensor]
            logging.debug("  POST %s to %s", json.dumps(data[sensor]), url)
            r = requests.post(url, json=data[sensor], headers = auth_header)
            logging.debug("  POST status: %d", r.status_code)
    else:
        logging.warn("  invalid device: %s", msg.dev_id)


def get_refresh_token(url, client, secret, username, password):
    # encode id in base64
    id=client+':'+secret
    id64 = base64.b64encode(id.encode())
    # create header and body for request
    header = {"Authorization":"Basic {}".format(id64.decode('ascii'))}
    body = {"grant_type":'password', "username": username, "password": password, "scope":"offline_access"}
    # send request and return token
    data = requests.post(url, data = body, headers = header)
    data_json=json.loads(data.content)
    logging.debug("REFRESH TOKEN: %s", data_json)
    return data_json['refresh_token']


def get_access_token(url, client, secret, token):
    # encode id in base64
    id=client+':'+secret
    id64 = base64.b64encode(id.encode())
    # create header and body for request
    header = {"Authorization":"Basic {}".format(id64.decode('ascii'))}
    body = {"grant_type":'refresh_token', "refresh_token": token}
    ## Token Access Point des Keycloak Servers um die Tokens zu bekommen.
    url = "https://udh-hh-tls.germanynortheast.cloudapp.microsoftazure.de/auth/realms/frostrealm/protocol/openid-connect/token"
    # send request and return token
    data = requests.post(url, data = body, headers = header)
    data_json=json.loads(data.content)
    logging.debug("ACCESS TOKEN: %s", data_json)
    return data_json['access_token']


def get_auth_header(token):
    header = {"Authorization": 'Bearer {}'.format(token)}
    return header

#    at_get = {"Authorization": 'Bearer {}'.format(at)}
#    return at_get

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('secrets', type=str,
                        help="JSON file with secrets for TTN and KeyCloak.")
    parser.add_argument('datastreams',  type=str,
                        help="JSON file with OGC datastream URLs.")
    args = parser.parse_args()

    with open(args.secrets, "r") as f:
        secrets = json.loads(f.read())

    # create auth stuff
    if 'keycloak' in secrets:
        kc = secrets['keycloak']
        rt = get_refresh_token(kc['url'], kc['id_client'], kc['id_secret'], kc['username'], kc['password'])
        at = get_access_token(kc['url'], kc['id_client'], kc['id_secret'], rt)
        global auth_header
        auth_header = get_auth_header(at)
        logging.debug("AUTH HEADER: %s", json.dumps(auth_header))
    else:
        logging.warn("No KeyCloak credentials found, skipping ...")

    if 'ttn' not in secrets:
        logging.critical("No TTN credentials found!")
        exit(1)

    with open(args.datastreams, "r") as f:
        global datastreams
        datastreams = json.loads(f.read())

    logging.debug("URLS: ", json.dumps(datastreams, indent=2))
    with open(f_logging, 'a') as fl:
        print(json.dumps(datastreams), file = fl)

    # connect to TTN
    ttncli = ttn.HandlerClient(secrets['ttn']['app_id'], secrets['ttn']['app_key'])

    mqttcli = ttncli.data()
    mqttcli.set_uplink_callback(uplink_callback)
    mqttcli.connect()

    # keep program alive and wait for uplink callbacks
    while 1:
        sleep(10)


if __name__ == "__main__":
    main()
