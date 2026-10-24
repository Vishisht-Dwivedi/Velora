import unittest

from mtcp_protocol_client import (
    CONNECT,
    CONNECT_ACK,
    HEADER,
    MAGIC,
    PONG,
    ProtocolTestError,
    VERSION,
    encode_packet,
    receive_response,
)


class FakeSocket:
    def __init__(self, data, chunk_size):
        self.data = bytearray(data)
        self.chunk_size = chunk_size

    def recv(self, length):
        if not self.data:
            return b""
        chunk_length = min(length, self.chunk_size, len(self.data))
        chunk = bytes(self.data[:chunk_length])
        del self.data[:chunk_length]
        return chunk


class ProtocolClientTests(unittest.TestCase):
    def test_encode_connect_header(self):
        self.assertEqual(encode_packet(CONNECT), HEADER.pack(MAGIC, VERSION, CONNECT, 0, 0, 0))

    def test_receive_response_across_partial_reads(self):
        response = HEADER.pack(MAGIC, VERSION, CONNECT_ACK, 0, 0, 0)
        receive_response(FakeSocket(response, 2), CONNECT_ACK)

    def test_rejects_wrong_response_type(self):
        response = HEADER.pack(MAGIC, VERSION, PONG, 0, 0, 0)
        with self.assertRaises(ProtocolTestError):
            receive_response(FakeSocket(response, HEADER.size), CONNECT_ACK)

    def test_rejects_nonzero_payload(self):
        response = HEADER.pack(MAGIC, VERSION, PONG, 0, 0, 1)
        with self.assertRaises(ProtocolTestError):
            receive_response(FakeSocket(response, HEADER.size), PONG)

    def test_rejects_eof(self):
        with self.assertRaises(ProtocolTestError):
            receive_response(FakeSocket(b"", HEADER.size), PONG)


if __name__ == "__main__":
    unittest.main()