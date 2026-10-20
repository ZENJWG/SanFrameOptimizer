import tempfile
import unittest
from pathlib import Path
from compare_framestats import read, report

class Tests(unittest.TestCase):
    def test_parser_and_weighted_average(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "test.log"
            p.write_text("FrameStats: samples=100, avg=10.000 ms, fps=100, p95=12.000 ms, p99=15.000 ms\n"
                         "FrameStats: samples=300, avg=20.000 ms, fps=50, p95=25.000 ms, p99=30.000 ms\n")
            result = report(p)
            self.assertEqual(result["frames"], 400)
            self.assertAlmostEqual(result["estimated_fps"], 1000/17.5)

    def test_empty_file(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "empty.log"
            p.write_text("nothing here")
            self.assertEqual(read(p), [])

if __name__ == "__main__":
    unittest.main()
