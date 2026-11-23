"""impo_cloud_route.c, the mainland China relay decision, on the host."""

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class CloudRouteHarnessTest(unittest.TestCase):
    def build_and_run(self, *defines: str) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            self.skipTest("C compiler not available")
        with tempfile.TemporaryDirectory() as tmpdir:
            binary = Path(tmpdir) / "cloud_route_harness"
            compile_proc = subprocess.run(
                [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", *defines,
                 "-I", str(ROOT / "tests" / "cloud_route_fakes"),
                 "-I", str(ROOT / "components" / "impo_cloud_route" / "include"),
                 str(ROOT / "tests" / "cloud_route_harness.c"),
                 str(ROOT / "components" / "impo_cloud_route" / "impo_cloud_route.c"),
                 "-o", str(binary)],
                text=True, capture_output=True)
            self.assertEqual(compile_proc.returncode, 0, compile_proc.stdout + compile_proc.stderr)
            run = subprocess.run([str(binary)], text=True, capture_output=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("cloud route harness: ok", run.stdout)

    def test_relay_on(self) -> None:
        self.build_and_run()

    def test_relay_off(self) -> None:
        self.build_and_run("-DROUTE_RELAY_OFF")

    def test_every_cloud_connection_takes_the_route(self) -> None:
        noise = (ROOT / "main/noise_control.cpp").read_text()
        chat = (ROOT / "components/impo/impo_chat_session.cpp").read_text()
        api = (ROOT / "main/vm_api.c").read_text()
        account = (ROOT / "components/impo/impo_account_api.c").read_text()
        wifi = (ROOT / "main/wifi_mgr.c").read_text()
        self.assertIn("impo_cloud_route_endpoint(s_noise_host, s_noise_peer,", noise)
        self.assertIn("esp_tls_conn_new_sync(s_noise_peer,", noise)
        self.assertIn("impo_cloud_route_report(s_noise_peer, r == 1);", noise)
        self.assertIn("NOISE_PATH, s_vm_id, s_noise_host_header,", noise)
        self.assertIn("impo_cloud_route_endpoint(s_host, s_peer,", chat)
        self.assertIn("esp_tls_conn_new_sync(s_peer,", chat)
        self.assertIn("id, s_host_header, token);", chat)
        self.assertIn("impo_cloud_route_url(s_api_base, base, sizeof(base));", api)
        self.assertIn("impo_cloud_route_report(url,", api)
        self.assertIn("impo_cloud_route_url(FETCH_URL, url, sizeof(url));", account)
        self.assertIn("impo_cloud_route_report(url,", account)
        self.assertIn("impo_cloud_route_reset();", wifi)
        for source in (noise, chat):
            self.assertNotIn("NOISE_PORT", source)


if __name__ == "__main__":
    unittest.main()
