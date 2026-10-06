import importlib.util
from pathlib import Path
import unittest
from unittest.mock import Mock, patch

spec = importlib.util.spec_from_file_location('provision', Path(__file__).parents[1] / 'tools/provision.py')
p = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p)


class ProvisionTests(unittest.TestCase):
    def test_mqtt_prompt_sequence_and_no_password_output(self):
        serial = Mock()
        fields = [('MQTT_HOST_REQUEST', 'broker.example'), ('MQTT_PORT_REQUEST', '8883'),
                  ('MQTT_USERNAME_REQUEST', 'example'), ('MQTT_PASSWORD_REQUEST', 'test-only')]
        serial.readline.side_effect = [b'boot banner\n'] + [(key+'\n').encode() for key, _ in fields] + [b'MQTT_CONNECTED\n']
        with patch('builtins.print') as output:
            p.exchange(serial, 'MQTT', fields)
        self.assertEqual([c.args[0] for c in serial.write.call_args_list],
                         [b'MQTT_SETUP\n'] + [(value+'\n').encode() for _, value in fields])
        output.assert_not_called()

    def test_failed_setup_does_not_expose_device_reply(self):
        serial = Mock()
        serial.readline.return_value = b'MQTT_ERROR untrusted reply\n'
        with self.assertRaisesRegex(RuntimeError, '^Device rejected setup;'):
            p.exchange(serial, 'MQTT', [])

    def test_input_rejects_protocol_injection_and_byte_overflow(self):
        for value in ['a\nb', 'a\rb', 'a\0b', 'é'*17]:
            with self.assertRaises(ValueError):
                p.checked(value, 32)
        self.assertEqual(p.checked('', 63, 0), '')

    def test_timeout_is_bounded(self):
        with patch.object(p.time, 'monotonic', side_effect=[0, 91]):
            with self.assertRaises(TimeoutError):
                p.exchange(Mock(), 'WIFI', [])


if __name__ == '__main__':
    unittest.main()
