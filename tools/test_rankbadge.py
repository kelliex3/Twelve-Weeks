"""python test_rankbadge.py - the rank table wire format and the name key."""
import unittest
import rankbadge as R


class T(unittest.TestCase):
    def test_norm(self):
        self.assertEqual(R.norm("[ABC] Jo e!"), "abcjoe")
        self.assertEqual(R.norm("_ABC__Jo_e_"), "abcjoe")
        self.assertEqual(R.norm("***"), "")

    def test_key_stable(self):
        self.assertEqual(R.key("[X] Ann"), R.key("_X__ann"))
        self.assertEqual(R.key("a"), (2166136261 ^ 97) * 16777619 & 0xFFFFFFFF)
        self.assertNotEqual(R.key("---"), 0)          # no letters / digits: keyed on its other bytes (was 0 = no badge)
        self.assertEqual(R.key("   "), 0)
        self.assertEqual(R.key(">,..,<"), R.key(",..,"))  # the daemon's safe_name strips < > - same key
        self.assertEqual(R.key("_--_-_"), R.key("---"))   # the vote page's "_" spacing
        self.assertNotEqual(R.key("Мирон"), 0)
        self.assertNotEqual(R.key(">,..,<"), R.key("Мирон"))

    def test_wire(self):
        t = R.wire([("Ann", 1, 12), ("ANN", 3, 4), ("   ", 0, 0), ("---", 2, 7), ("Bo", 99, 5000)])
        p = t.split()
        self.assertEqual(p[0], "n=3")
        self.assertEqual(p[1], "%08x:1:12" % R.key("Ann"))
        self.assertEqual(p[2], "%08x:2:7" % R.key("---"))
        self.assertEqual(p[3], "%08x:9:999" % R.key("Bo"))


if __name__ == "__main__":
    unittest.main()
