"""Actual broker dispatcher and memfd wire exchange, with a host Vulkan mock."""
import array
import mmap
import os
import socket
import struct

from test_normal_launch import NormalBrokerTests
from test_renderer import RendererFixture


class SharedBrokerTests(RendererFixture):
    open_normal = NormalBrokerTests.open_normal

    def register(self, connection, device, size=4096):
        memory = self.new(connection, 35, device, suffix=struct.pack('<QI', size, 1))
        self.fields(connection, 38, device, memory, suffix=struct.pack('<QQ', 0, size))
        self.fields(connection, 93, device, memory, suffix=struct.pack('<Q', size))
        envelope, ancillary, flags, _ = connection.recvmsg(1, socket.CMSG_SPACE(4), socket.MSG_CMSG_CLOEXEC)
        descriptors = array.array('i')
        for level, kind, data in ancillary:
            if (level, kind) == (socket.SOL_SOCKET, socket.SCM_RIGHTS):
                descriptors.frombytes(data)
        try:
            self.assertEqual((envelope, flags & socket.MSG_CTRUNC, len(descriptors)), (b'S', 0, 1))
            self.assertFalse(os.get_inheritable(descriptors[0]))
            mapping = mmap.mmap(descriptors[0], 0, flags=mmap.MAP_SHARED, prot=mmap.PROT_READ | mmap.PROT_WRITE)
        finally:
            for descriptor in descriptors:
                os.close(descriptor)
        self.addCleanup(mapping.close)
        return memory, mapping

    def test_register_notify_ack_readback_unmap_and_fresh_sequence(self):
        with self.broker(43) as path:
            connection, device = self.open_normal(path)
            self.fields(connection, 87, device, 0)
            for _ in range(2):
                memory, mapping = self.register(connection, device)
                pixels = b'phase6-shared-pixels'
                mapping[32:32 + len(pixels)] = pixels
                struct.pack_into('<Q', mapping, 4096, 1)
                self.fields(connection, 94, device, memory, suffix=struct.pack('<QQQ', 32, len(pixels), 1))
                readback = self.fields(connection, 40, device, memory,
                                       suffix=struct.pack('<QI', 32, len(pixels)), expected=(0, 0, 1))
                self.assertEqual(readback, pixels)
                self.fields(connection, 95, device, memory)
                mapping.close()
                self.fields(connection, 39, device, memory)
                self.fields(connection, 36, device, memory)
            self.fields(connection, 90, device)
            self.fields(connection, 16, device)
        self.assertIn('baseline: devices=0 queues=0', self.broker_errors)

    def test_sequence_rejection_disconnect_and_restart_release_cpu_maps(self):
        for _ in range(2):
            with self.broker(43) as path:
                connection, device = self.open_normal(path)
                self.fields(connection, 87, device, 0)
                memory, mapping = self.register(connection, device)
                struct.pack_into('<Q', mapping, 4096, 2)
                self.fields(connection, 94, device, memory, suffix=struct.pack('<QQQ', 0, 4, 2), expected=(3, 0, 0))
                self.assert_disconnected(connection)
                mapping.close()
            self.assertIn('baseline: devices=0 queues=0', self.broker_errors)
