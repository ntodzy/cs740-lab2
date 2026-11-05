import importlib.util
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET

SPEC = importlib.util.spec_from_file_location("combine_graphs", Path(__file__).parents[1] / "combine_graphs.py")
combine = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(combine)
NS = {"s": combine.SVG}


class CombineGraphsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.originals = {}
        for workload in combine.WORKLOADS:
            for category in combine.CATEGORIES:
                path = self.directory / f"{workload}-{category}.svg"
                content = (f'<svg xmlns="{combine.SVG}" width="100" height="50" viewBox="0 0 100 50">'
                    f'<title>{workload}-{category}</title><defs><clipPath id="clip"><rect width="10" height="10"/></clipPath></defs>'
                    '<path id="curve" clip-path="url(#clip)" d="M0 0 L10 10"/>'
                    '<use href="#curve"/></svg>')
                path.write_text(content)
                self.originals[path] = content

    def test_three_columns_four_rows_and_original_data_preserved(self):
        output = self.directory / "combined.svg"
        self.assertEqual(combine.combine(self.directory, output, gap=10), 12)
        root = ET.parse(output).getroot()
        self.assertEqual(root.get("viewBox"), "0 0 340 250")
        panels = root.findall("s:svg", NS)
        self.assertEqual(len(panels), 12)
        for i, panel in enumerate(panels):
            self.assertEqual(panel.get("x"), str(10 + i % 3 * 110))
            self.assertEqual(panel.get("y"), str(10 + i // 3 * 60))
            self.assertEqual(panel.find("s:title", NS).text,
                f"{combine.WORKLOADS[i // 3]}-{combine.CATEGORIES[i % 3]}")
            self.assertEqual(panel.find("s:path", NS).get("d"), "M0 0 L10 10")
            self.assertEqual(panel.find("s:path", NS).get("clip-path"), f"url(#panel-{i}-clip)")
            self.assertEqual(panel.find("s:use", NS).get("href"), f"#panel-{i}-curve")
        ids = [element.get("id") for element in root.iter() if element.get("id")]
        self.assertEqual(len(ids), len(set(ids)))
        for path, text in self.originals.items():
            self.assertEqual(path.read_text(), text)

    def test_single_workload_and_missing_input(self):
        output = self.directory / "one.svg"
        self.assertEqual(combine.combine(self.directory, output, ("pareto",)), 3)
        (self.directory / "uniform-all.svg").unlink()
        with self.assertRaises(FileNotFoundError):
            combine.combine(self.directory, self.directory / "missing.svg")
        self.assertFalse((self.directory / "missing.svg").exists())

    def test_refuses_to_overwrite_source(self):
        source = self.directory / "uniform-all.svg"
        with self.assertRaisesRegex(ValueError, "replace an input"):
            combine.combine(self.directory, source)
        self.assertEqual(source.read_text(), self.originals[source])

    def test_separate_rows_cli(self):
        output = self.directory / "rows.svg"
        self.assertEqual(combine.main(["--input", str(self.directory), "--output", str(output),
                                       "--separate-rows"]), 0)
        self.assertFalse(output.exists())
        for workload in combine.WORKLOADS:
            panels = ET.parse(self.directory / f"rows-{workload}.svg").getroot().findall("s:svg", NS)
            self.assertEqual(len(panels), 3)
            self.assertEqual(len({panel.get('y') for panel in panels}), 1)
            self.assertEqual([panel.find('s:title', NS).text for panel in panels],
                             [f"{workload}-{category}" for category in combine.CATEGORIES])


if __name__ == "__main__":
    unittest.main()
