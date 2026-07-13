// Browser-only entry point. The flasher and editor share this connector so
// Frothy's serial transcript has one parser and one close path.

export { createConnector } from "./connector.ts";
export { WebSerialTransport, type WebSerialPort } from "./web-serial.ts";
export {
  TransportError,
  WireFormatError,
  type ReplConnector,
  type Response,
  type Status,
  type Transport,
  type Unsubscribe,
} from "./types.ts";
