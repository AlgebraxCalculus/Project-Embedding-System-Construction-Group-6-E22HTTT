import Paho from 'paho-mqtt';

const DEFAULT_URL = 'wss://e4b01f831a674150bbae2854b6f1735c.s1.eu.hivemq.cloud:8884/mqtt';
const DEFAULT_DEVICE_ID = 'petfeeder-feed-node-01';
const DEFAULT_USERNAME = 'quandotrung';
const DEFAULT_PASSWORD = 'Pass1235';

export const createMqttClient = ({ deviceId, onTelemetry, onAck, onAlert, onStatusChange } = {}) => {
  const brokerUrl = process.env.EXPO_PUBLIC_MQTT_URL || DEFAULT_URL;
  const resolvedDeviceId = deviceId || process.env.EXPO_PUBLIC_DEVICE_ID || DEFAULT_DEVICE_ID;
  const clientId = `spf-mobile-${Math.random().toString(16).slice(2)}`;
  const username = process.env.EXPO_PUBLIC_MQTT_USERNAME || DEFAULT_USERNAME;
  const password = process.env.EXPO_PUBLIC_MQTT_PASSWORD || DEFAULT_PASSWORD;

  // Parse host and port from the broker URL
  const urlObj = new URL(brokerUrl);
  const host = urlObj.hostname;
  const port = parseInt(urlObj.port, 10) || 8884;
  const path = urlObj.pathname || '/mqtt';

  const client = new Paho.Client(host, port, path, clientId);

  const topics = [
    `feeder/${resolvedDeviceId}/telemetry`,
    `feeder/${resolvedDeviceId}/ack`,
    `feeder/${resolvedDeviceId}/alert`,
  ];

  client.onConnectionLost = (resp) => {
    if (resp.errorCode !== 0) {
      onStatusChange?.('offline');
      // Attempt reconnect after delay
      setTimeout(() => doConnect(), 5000);
    }
  };

  client.onMessageArrived = (message) => {
    const topic = message.destinationName;
    const payloadString = message.payloadString;
    let data;
    try {
      data = JSON.parse(payloadString);
    } catch {
      data = { raw: payloadString, message: payloadString };
    }

    if (topic.includes('/telemetry')) onTelemetry?.(data, payloadString);
    else if (topic.includes('/ack')) onAck?.(data, payloadString);
    else if (topic.includes('/alert')) onAlert?.(data, payloadString);
  };

  function doConnect() {
    onStatusChange?.('reconnecting');
    client.connect({
      useSSL: true,
      userName: username,
      password,
      cleanSession: true,
      timeout: 6,
      onSuccess: () => {
        onStatusChange?.('online');
        topics.forEach((t) => client.subscribe(t));
      },
      onFailure: (err) => {
        console.warn('MQTT connect failed:', err.errorMessage);
        onStatusChange?.('offline');
        setTimeout(() => doConnect(), 5000);
      },
    });
  }

  doConnect();

  // Return a wrapper with an interface compatible with the rest of the app
  return {
    end: () => {
      if (client.isConnected()) client.disconnect();
    },
    publish: (topic, message) => {
      if (client.isConnected()) {
        const msg = new Paho.Message(typeof message === 'string' ? message : JSON.stringify(message));
        msg.destinationName = topic;
        client.send(msg);
      }
    },
    connected: () => client.isConnected(),
    _paho: client,
  };
};
