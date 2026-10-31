import unittest

from dktools import typeaudit


class ConceptTests(unittest.TestCase):
    def test_a_name_means_its_last_word(self):
        cases = {
            "timeoutMs": "ms",
            "nodeId": "id",
            "maxLevels": "level",
            "hotMaxBytes": "byte",
            "key_len": "len",
            "flags": "flag",
            "status": "status",
            "us": "us",
            "x": "x",
            "rowCap": "cap",
            "replicaCount": "count",
        }
        for name, concept in cases.items():
            with self.subTest(name=name):
                self.assertEqual(typeaudit.concept(name), concept)


class DeclarationTests(unittest.TestCase):
    def test_a_prototype_gives_its_integer_parameters_and_return(self):
        sites = typeaudit.prototype_sites(
            "src/a.h",
            "fooSet",
            "uint64_t fooSet(foo *f, const uint32_t level, size_t *outBytes, "
            "const char *name, int16_t deltas[], double ratio)",
        )
        self.assertEqual(
            [(s.owner, s.name, s.type, s.pointer) for s in sites],
            [
                ("fooSet", "fooSet", "uint64_t", False),
                ("fooSet", "level", "uint32_t", False),
                ("fooSet", "outBytes", "size_t", True),
                ("fooSet", "deltas", "int16_t", True),
            ],
        )

    def test_void_bool_and_pointer_returns_are_not_integers(self):
        for proto in (
            "void fooFree(foo *f)",
            "bool fooHas(const foo *f, int key)",
            "uint8_t *fooBytes(const foo *f)",
        ):
            with self.subTest(proto=proto):
                sites = typeaudit.prototype_sites("src/a.h", "f", proto)
                self.assertTrue(all(s.owner != s.name for s in sites), sites)

    def test_struct_fields_with_several_declarators_and_bitfields(self):
        sites = typeaudit.field_sites(
            "src/a.h",
            "fooStats",
            "uint64_t hits, misses;\n"
            "    uint32_t width : 16;\n"
            "    const char *label;\n"
            "    size_t sizes[4];\n"
            "    struct { int inner; } nested;\n",
        )
        self.assertEqual(
            [(s.name, s.type) for s in sites],
            [
                ("hits", "uint64_t"),
                ("misses", "uint64_t"),
                ("width", "uint32_t"),
                ("sizes", "size_t"),
            ],
        )


class GroupTests(unittest.TestCase):
    def test_concepts_group_with_their_type_spread(self):
        sites = [
            typeaudit.Site("src/a.h", "f", "level", "size_t", False),
            typeaudit.Site("src/b.h", "g", "maxLevels", "uint32_t", False),
            typeaudit.Site("src/b.h", "S", "levels", "size_t", False),
            typeaudit.Site("src/b.h", "S", "ttlMs", "uint64_t", False),
        ]
        groups = typeaudit.group(sites)
        self.assertEqual([g.concept for g in groups], ["level", "ms"])
        self.assertEqual(groups[0].types, {"size_t": 2, "uint32_t": 1})
        text = typeaudit.render(groups, min_sites=1)
        self.assertIn("level  3 sites  size_t 2, uint32_t 1", text)


if __name__ == "__main__":
    unittest.main()
