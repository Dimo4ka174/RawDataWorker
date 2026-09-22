# HTTP Protocol — RawDataWorker ↔ AWM

This document describes the wire contract between the Arduino firmware
(`RawDataWorker`) and the AWM backend.

- **Transport:** plain HTTP/1.1 over TCP
- **Direction:** Arduino → AWM (one-way from the client's perspective)
- **Default endpoint:** `POST /api/Wincc/updateMachineStatus`
- **Content type:** `application/json`
- **Encoding:** UTF-8, no BOM

> ⚠️ There is no TLS and no authentication. The device is expected to run
> on an isolated industrial LAN. If you expose the endpoint to a broader
> network, put it behind a reverse proxy with authentication.

---

## 1. Endpoint

### `POST /api/Wincc/updateMachineStatus`

Called by the Arduino whenever the debounced machine state differs from
the last state that was successfully delivered.

#### Request headers

| Header           | Value              | Notes                          |
| ---------------- | ------------------ | ------------------------------ |
| `Host`           | `<server-ip>`      | Plain IP, no DNS               |
| `Content-Type`   | `application/json` |                                |
| `Content-Length` | `<n>`              | Length of the JSON body        |
| `Connection`     | `close`            | One request per TCP connection |

#### Request body

```json
{
  "machineName": "Danobat",
  "Condition": true,
  "current": 3.42,
  "power": 787
}
```

| Field         | Type    | Units | Description                                  |
| ------------- | ------- | ----- | -------------------------------------------- |
| `machineName` | string  | —     | Logical machine identifier known by AWM      |
| `Condition`   | boolean | —     | `true` = working, `false` = idle             |
| `current`     | number  | A     | RMS current, two decimals                    |
| `power`       | number  | W     | Estimated apparent power `U · I`, integer    |

`Condition` is capitalised to match the existing AWM DTO
(`DemoStatusDto.Condition`). The other fields are lower-case for
historical reasons; the backend treats them case-insensitively.

#### Response

`AWM` returns `200 OK` for every successfully parsed request. The
response body carries a hint about whether the state actually changed:

```json
{ "changed": true }
```

| Body                     | Meaning                                            |
| ------------------------ | -------------------------------------------------- |
| `{"changed":true}`       | Server accepted the update and the state changed   |
| `{"changed":false}`      | Server accepted the update, but the state was same |
| *(anything else)* + 200  | Accepted; firmware treats it as success            |

The Arduino considers the delivery successful if **either**:

- the HTTP status line contains `200 OK`, **or**
- the body contains `"changed":true` / `"changed":false`.

Any other outcome (timeout, connection refused, non-200 response) is
treated as a failure and logged to the serial console. The firmware does
**not** retry on its own — instead it waits for the next state change and
sends again.

---

## 2. Timing

| Parameter                        | Value      | Source                       |
| -------------------------------- | ---------- | ---------------------------- |
| Sampling period                  | 1000 ms    | `MEASURE_INTERVAL_MS`        |
| Debounce window                  | 3 samples  | `NEED_STABLE_READINGS`       |
| HTTP socket timeout              | 2000 ms    | `HTTP_TIMEOUT_MS`            |
| Effective worst-case latency     | ~5 s       | measure + debounce + timeout |

While the HTTP request is in flight, the sampling loop is paused. At a
1 Hz cadence this is acceptable. If a higher sampling rate is ever
required, the HTTP layer must be reworked into a non-blocking state
machine.

---

## 3. Example exchange

Full request as it appears on the wire (`\r\n` shown explicitly):

```
POST /api/Wincc/updateMachineStatus HTTP/1.1\r\n
Host: 192.168.10.200\r\n
Content-Type: application/json\r\n
Content-Length: 87\r\n
Connection: close\r\n
\r\n
{"machineName":"Danobat","Condition":true,"current":3.42,"power":787}
```

Typical response:

```
HTTP/1.1 200 OK\r\n
Content-Type: application/json\r\n
Content-Length: 18\r\n
Connection: close\r\n
\r\n
{"changed":true}
```

---

## 4. Error handling

| Scenario                          | Firmware behaviour                             |
| --------------------------------- | ---------------------------------------------- |
| `connect()` fails                 | Log `ERR: connect failed`, skip cycle          |
| TCP connects, no response in 2 s  | Log `ERR: no 200 OK`, close socket, skip cycle |
| Response is not `200 OK`          | Same as above                                  |
| Response is `200 OK`              | Log `[CHANGED]` or `[UNCHANGED]`, update state |
| Ethernet cable unplugged          | `Ethernet.localIP() == 0.0.0.0` at boot → halt |

There is no exponential backoff and no persistent retry queue. The
device only reports state *changes*, so a missed delivery will be
retried on the next change anyway. This is a deliberate design choice to
keep the firmware small and predictable.

---

## 5. Versioning

The protocol is not versioned. Any change to the JSON shape requires
updating the firmware and the backend DTO together. For a system of
this size, adding a `version` field would be over-engineering.

---

## 6. Reference implementation

- Firmware: [`firmware/RawDataWorker/RawDataWorker.ino`](../firmware/RawDataWorker/RawDataWorker.ino)
- Backend: `AWM/Controllers/WinccController` (private repository)