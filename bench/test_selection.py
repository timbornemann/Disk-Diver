import unittest

from select_core import CASES, select


def reports(cpp, rust, qualified=True):
    result = []
    for n, system in enumerate(("Windows", "Linux")):
        cases = {name: {core: [{"wall_ms": values[n * 3 + i]}] for core, values in (("cpp", cpp), ("rust", rust))} for i, name in enumerate(CASES)}
        result.append({"os": system, "qualified": qualified, "worker_sha256": "paired", "cases": cases})
    return result


class Selection(unittest.TestCase):
    def test_mismatched_coordinator(self):
        pair = reports([100]*6, [100]*6)
        pair[0]["worker_sha256"] = "different"
        self.assertIsNone(select(pair)["winner"])

    def test_small_fixture_never_selects(self):
        self.assertIsNone(select(reports([100]*6, [200]*6, False))["winner"])

    def test_only_eligible_core(self):
        self.assertEqual(select(reports([100]*6, [90]*5+[116]))["winner"], "cpp")

    def test_thresholds_and_near_tie(self):
        self.assertEqual(select(reports([109]*6,[100]*6))["winner"], "cpp")
        self.assertEqual(select(reports([110]*6,[100]*6))["winner"], "rust")
        self.assertEqual(select(reports([115]*6,[100]*6))["winner"], "rust")

    def test_profile_then_minimax(self):
        mixed = reports([100, 118]*3, [123, 100]*3)
        self.assertIsNone(select(mixed)["winner"])
        self.assertEqual(select(mixed, after_profile=True)["winner"], "cpp")


if __name__ == "__main__":
    unittest.main()
