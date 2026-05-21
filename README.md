# fixclient

## Sequence token format

The client now keeps both sequence directions in the same daily token file:

```text
tokens/<SenderCompID>_<YYYYMMDD>.token
0000000016 : 0000000035
```

Meaning:

```text
left  = next outbound MsgSeqNum sent by the client
right = next inbound MsgSeqNum expected from JNX
```

Examples:

```text
0000000050 : 0000000035
```

Use this to test JNX inbound sequence handling. The client sends the next outbound message with `34=50`.

```text
0000000016 : 0000000050
```

Use this to test client inbound sequence handling. The client expects the next JNX message to be `34=50`; if JNX sends a lower sequence number, the client detects the error and disconnects.

## Resend support

The client now supports:

- inbound `MsgSeqNum(34)` validation
- `ResendRequest(35=2)` generation when an inbound gap is detected
- `SequenceReset(35=4)` / GapFill handling
- sent-message storage during the current run
- replying to JNX `ResendRequest(35=2)`
- resending application messages with `PossDupFlag(43)=Y` and `OrigSendingTime(122)`

This is intended for conformance tests #4, #5, #6, and #7.

## Persistent resend store

This version writes every sent FIX message into a resend store file next to the token file.

Example:

```text
tokens/TX99900B_20260522.token
0000000005 : 0000000008

tokens/TX99900B_20260522.resend
3|383D4649582E342E342E2E2E
```

The `.resend` file is used when JNX sends a ResendRequest after the tool is restarted. If JNX requests an old application message, the client can reload it from the `.resend` file and resend it with `PossDupFlag(43)=Y` and `OrigSendingTime(122)`.
