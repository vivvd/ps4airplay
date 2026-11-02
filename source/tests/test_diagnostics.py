#!/usr/bin/env python3
"""Integration tests against the actual portable HTTP server, not a mock."""
import http.client
import json
from pathlib import Path
import socket
import subprocess
import time
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class DiagnosticsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(['sh', str(ROOT / 'scripts/build_host.sh')], check=True)
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            cls.port = sock.getsockname()[1]
        cls.profile_workspace=tempfile.TemporaryDirectory(prefix='ps4-profile-http-')
        (Path(cls.profile_workspace.name)/'build').mkdir()
        cls.process = subprocess.Popen([str(ROOT / 'build/receiver-host'), '--port', str(cls.port)],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,cwd=cls.profile_workspace.name)
        for _ in range(100):
            try:
                with socket.create_connection(('127.0.0.1', cls.port), timeout=.2): break
            except OSError: time.sleep(.02)
        else: raise RuntimeError('Server did not start')

    @classmethod
    def tearDownClass(cls):
        cls.process.terminate()
        _, err = cls.process.communicate(timeout=5)
        cls.profile_workspace.cleanup()
        if cls.process.returncode != 0 or err:
            raise AssertionError(f'Host server failed: {cls.process.returncode}, {err.decode()}')

    def request(self, method, path, headers=None):
        conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=3)
        conn.request(method, path, body=b'' if method == 'POST' else None, headers=headers or {})
        response = conn.getresponse()
        body = response.read()
        status, fields = response.status, dict(response.getheaders())
        conn.close()
        return status, fields, body

    def raw(self, fragments):
        with socket.create_connection(('127.0.0.1', self.port), timeout=3) as sock:
            for fragment in fragments:
                sock.sendall(fragment)
                time.sleep(.01)
            response = b''
            while True:
                try:
                    part = sock.recv(65536)
                except ConnectionResetError:
                    # Oversized unread input can cause TCP reset on close,
                    # after the server has sent the complete 400 response.
                    if response: return response
                    raise
                if not part: return response
                response += part

    def test_status_does_not_claim_hardware_or_airplay(self):
        status, _, body = self.request('GET', '/api/status')
        self.assertEqual(status, 200)
        info = json.loads(body)
        self.assertEqual(info['platform'], 'host-simulation')
        self.assertFalse(info['airplay_ready'])
        self.assertFalse(info['h264_decode_tested'])
        self.assertEqual(info['audio_rc'], -1)

    def test_page_and_report(self):
        status, fields, body = self.request('GET', '/')
        self.assertEqual(status, 200)
        self.assertIn('text/html', fields['Content-Type'])
        self.assertIn('Скачать отчёт', body.decode())
        status, fields, body = self.request('GET', '/api/report')
        self.assertEqual(status, 200)
        self.assertIn('attachment', fields['Content-Disposition'])
        self.assertIn('log', json.loads(body))
        self.assertEqual(int(fields['Content-Length']), len(body))

    def test_language_switch_updates_app_status(self):
        for language in ('ru', 'en'):
            status, _, _ = self.request('POST', '/api/language/' + language)
            self.assertEqual(status, 202)
            _, _, body = self.request('GET', '/api/status')
            self.assertEqual(json.loads(body)['language'], language)
        status, _, _ = self.request('POST', '/api/language/fr')
        self.assertEqual(status, 404)

    def test_commands_and_reconnect(self):
        for command, expected in [('pattern', True), ('reset', False)]:
            self.assertEqual(self.request('POST', '/api/' + command)[0], 202)
            self.assertEqual(json.loads(self.request('GET', '/api/status')[2])['pattern'], expected)
        self.assertEqual(self.request('POST', '/api/tone')[0], 202)
        time.sleep(.03)
        info = json.loads(self.request('GET', '/api/status')[2])
        self.assertIn('no PS4 audio output performed', info['log'])
        self.assertEqual(info['tone_runs'], 0)

    def test_manual_airplay_does_not_auto_start_on_host(self):
        self.assertEqual(self.request('POST', '/api/airplay-step')[0], 202)
        time.sleep(.03)
        state = json.loads(self.request('GET', '/api/status')[2])
        self.assertFalse(state['airplay_ready'])
        self.assertEqual(state['airplay_step'], 0)
        self.assertIn('PS4 hardware required', state['log'])

    def test_video_profile_changes_apply_after_restart(self):
        initial=json.loads(self.request('GET','/api/status')[2])
        self.assertEqual(initial['video_profile'],1)
        self.assertEqual(self.request('POST','/api/video-profile/720p60')[0],202)
        state=json.loads(self.request('GET','/api/status')[2])
        self.assertEqual(state['video_profile'],1)
        self.assertEqual(state['video_pending_profile'],2)
        self.assertEqual((Path(self.profile_workspace.name)/'build/host-video-profile.txt').read_text(),'720p60\n')
        self.assertEqual(self.request('POST','/api/video-profile/unknown')[0],400)
        self.assertEqual(self.request('POST','/api/video-profile/720p30',{'Origin':'https://example.invalid'})[0],403)
        self.assertEqual(self.request('GET','/api/video-profile/720p30')[0],404)
        self.assertEqual(self.request('POST','/api/video-profile/1080p30')[0],202)

    def test_embedded_video_and_ranges(self):
        expected=(ROOT/'assets/airplay-test.mp4').read_bytes()
        status,fields,body=self.request('GET','/test-video.mp4')
        self.assertEqual(status,200);self.assertEqual(body,expected)
        self.assertEqual(fields.get("Cache-Control"),"no-store")
        self.assertEqual(self.request("GET","/test-video.mp4?v=1.11-RC12")[2],expected)
        for value,start,end in [('bytes=0-31',0,32),('bytes=32-',32,len(expected)),('bytes=-16',len(expected)-16,len(expected))]:
            status,fields,body=self.request('GET','/test-video.mp4',{'Range':value})
            self.assertEqual(status,206);self.assertEqual(body,expected[start:end]);self.assertIn('Content-Range',fields)
        self.assertEqual(self.request('GET','/test-video.mp4',{'Range':'bytes=999999-'})[0],416)
        self.assertEqual(self.request('GET','/test-video.mp4',{'Range':'bytes=0-1,3-4'})[0],416)
        status,fields,body=self.request('HEAD','/test-video.mp4')
        self.assertEqual(status,200);self.assertEqual(body,b'');self.assertEqual(int(fields['Content-Length']),len(expected))

    def test_cross_origin_command_rejected(self):
        self.assertEqual(self.request('POST', '/api/tone', {'Origin': 'https://example.invalid'})[0], 403)
        self.assertEqual(self.request('POST', '/api/tone', {'Origin': f'http://127.0.0.1:{self.port}'})[0], 202)

    def test_get_cannot_trigger_tone(self):
        self.assertEqual(self.request('GET', '/api/tone')[0], 404)
        self.assertEqual(self.request('DELETE', '/api/status')[0], 405)

    def test_fragmented_headers(self):
        data = self.raw([b'GET /api/sta', b'tus HTTP/1.1\r\nHost: local\r\n', b'\r\n'])
        self.assertTrue(data.startswith(b'HTTP/1.1 200 '))
        json.loads(data.split(b'\r\n\r\n', 1)[1])

    def test_invalid_headers_and_nul(self):
        requests = [b'GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n',
                    b'POST /api/tone HTTP/1.1\r\nHost: a\r\nContent-Length: 20\r\n\r\n',
                    b'GET / HTTP/1.1\r\nHost: a\x00b\r\n\r\n',
                    b'GET / HTTP/1.1\r\nHost: a\r\nX: ' + b'x' * 4096]
        for request in requests:
            with self.subTest(request=request[:60]):
                self.assertTrue(self.raw([request]).startswith(b'HTTP/1.1 400 '))
        self.assertEqual(self.request('GET', '/api/status')[0], 200)

    def test_slow_clients_do_not_block_or_leak_slots(self):
        clients = [socket.create_connection(('127.0.0.1', self.port), timeout=3) for _ in range(3)]
        try:
            for sock in clients: sock.sendall(b'GET / HTTP/1.1\r\n')
            self.assertEqual(self.request('GET', '/api/status')[0], 200)
        finally:
            for sock in clients: sock.close()
        for _ in range(10):
            self.assertEqual(self.request('GET', '/api/status')[0], 200)

    def test_framebuffer_rendering_with_memory_checks(self):
        for pattern in (False, True):
            with tempfile.TemporaryDirectory() as temp:
                path = Path(temp) / 'screen.ppm'
                command = [str(ROOT / 'build/receiver-host'), '--snapshot', str(path)]
                if pattern: command.append('--pattern')
                result = subprocess.run(command, capture_output=True, check=True)
                self.assertFalse(result.stderr)
                data = path.read_bytes()
                header = b'P6\n1920 1080\n255\n'
                self.assertTrue(data.startswith(header))
                self.assertEqual(len(data) - len(header), 1920 * 1080 * 3)

if __name__ == '__main__':
    unittest.main(verbosity=2)
