"""s18 — tests for tools/remix_texhash.py (RTex Remix texture hashes from GTA SA .txd files).

Three layers:
  1. the bundled xxHash (0.8.0) reimplemented in python, checked against vectors produced by
     compiling the very xxhash.c that ships inside dxvk-remix 1.6.1 (see
     .strata/notes-s18-remix-texture-hash.md);
  2. the RenderWare texture-dictionary reader, on .txd files built in memory;
  3. the hashes the game really produced: a handful of rtx.conf entries and the mirror textures,
     read straight out of the (read-only) game folder. Skipped when the game is not installed.
"""

import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import remix_texhash as R  # noqa: E402

GAME = "/media/rhy/data/Gamez/GTASA-RTX"


def pattern(n):
    """The byte pattern the vectors below were made from: data[i] = (i*7+1) & 0xFF."""
    return bytes(((i * 7 + 1) & 0xFF) for i in range(n))


# (length, XXH64(data, 0), XXH3_64bits(data)) — generated with a C program that compiles
# dxvk-remix-1.6.1/src/util/xxHash/xxhash.c, i.e. the exact library Remix links.
VECTORS = [
    (0, "0xEF46DB3751D8E999", "0x2D06800538D394C2"),
    (1, "0x8A4127811B21E730", "0xE12EF9D2EB86CEEB"),
    (2, "0x2B319EC16221CA2B", "0xE406C97925067F24"),
    (3, "0xB6E6C910C2FD373A", "0x5C83885A0FB5D516"),
    (4, "0x22EDA2CF6AF4C124", "0x244F36DE481E7522"),
    (5, "0x1DA48E4B4CAFAB76", "0x7B9EBB96E74CF5A8"),
    (6, "0xFED587E892BC395F", "0x34D30E5777171DC1"),
    (7, "0x34084D91A233A751", "0x01ECDCA8DB6A5BB3"),
    (8, "0xC6F1803A5E0B3222", "0x96CC97A6768FD7A9"),
    (9, "0x9E8ADF2A0CCDB6DA", "0x4781D83B8E99D495"),
    (10, "0x3403ECD2E83631C2", "0xEA4ECA76A334572D"),
    (11, "0xA295D00465199F0A", "0x31174C3454C2DB6C"),
    (12, "0x9178B724DCE384C0", "0x88FB0A593142536C"),
    (13, "0xDFCED2D95E03C38E", "0xF2D44492D7AFD263"),
    (14, "0xFF34EE32A5C9ECFE", "0x0B347ECFAC13DF03"),
    (15, "0x514C6F58D37CE6F1", "0x2F8A10D8233170B7"),
    (16, "0xAFE8F989A0735A8E", "0x913BD4A8038027A7"),
    (17, "0xB191CD44210EA488", "0x2BF6F66973A6179D"),
    (18, "0x610796A2F9D71AB3", "0xEAAE295497523297"),
    (19, "0x58C2D9F72170FD3E", "0xFBF18B8CC2A0283A"),
    (20, "0x73BC78A4C4EBAA37", "0xDF3E21425A7D1C88"),
    (21, "0xF10004B1FFF00066", "0xD0618CB852617B53"),
    (22, "0xBFF028DD5D69B5D1", "0x5E99F3114D9B87F1"),
    (23, "0xC9316B87F4DB1C9F", "0xD557855E5B062571"),
    (24, "0x06C3C997B7008641", "0x9E3DC147F94568E6"),
    (25, "0x2D6288E385BB919F", "0xEAB6B13698E9ED1E"),
    (26, "0x417F6C5E6C3B15FA", "0x0210AB2695044BF4"),
    (27, "0xE66C81892928EAEE", "0xFC4E1DAB69AE1D85"),
    (28, "0x5C02532B751CF115", "0xA8E6A6537DFAD6A4"),
    (29, "0xA388F9B4D357CA62", "0x10E9CD0B42F02EF6"),
    (30, "0xDD06EEECA3DFA420", "0x539C42FEDD69E49E"),
    (31, "0x6AB1C40E29F50073", "0xD3E1646C0BAD5F21"),
    (32, "0x5A0756FBE9ECD3D1", "0x5D503B89AE6F56C6"),
    (33, "0xDC50CDC37BB9C183", "0xB192963EDE5ED5AB"),
    (34, "0xE0725237287738A3", "0x8FD5761FB8569335"),
    (35, "0x64593B59A48D806F", "0xD57C9F2FB0326984"),
    (36, "0x31E83081F3301BB8", "0x9A4097F6904EF445"),
    (37, "0x9F0A1BB6FDA206F1", "0x3C03D23F94BA57DB"),
    (38, "0x3CFE31931FB9630A", "0x6DCF6F31A93313D2"),
    (39, "0xE6B392C5985D77AB", "0x34F78409AB3FFA03"),
    (40, "0x7899CACB89A78430", "0xE819C54D0558B405"),
    (41, "0x8282A61B6BE9C67C", "0xC001FA976773FBE6"),
    (42, "0x401BF5A8B9B59765", "0x5C91F4639BDF87E2"),
    (43, "0x77F1A15C4A1E867E", "0x5148D00F85C2E25D"),
    (44, "0x122ACA1412911665", "0xC205039FDBDDE205"),
    (45, "0x50A9D30CF5044636", "0xE91A69ECBA875F22"),
    (46, "0x2EED34F9B495BA3B", "0x67DE84C17DEEADEC"),
    (47, "0xDDA7573AF4285E5C", "0xB719E1623A30F246"),
    (48, "0x6DD1908CBB9D1576", "0x21C9A73051F333E7"),
    (49, "0x3A71AEAB85ED6E00", "0xFD1F70AF34E48537"),
    (50, "0x63A3AC6ED3BCBC55", "0xDE56282FD3BC0E03"),
    (51, "0xF3B45F9CC5062F16", "0x1D0D817901FC64C2"),
    (52, "0xDAE39FC68C7734B1", "0xE1CE000252070780"),
    (53, "0x576B24A0569C2674", "0xDBF87D33F55340D3"),
    (54, "0x011DEFDCEB9FCDFD", "0x6D91A7C6FBC4C721"),
    (55, "0x14D336135EF6949D", "0x7A58428F673A170C"),
    (56, "0xF82C99F5332BA97D", "0x54C97E8B807D8950"),
    (57, "0x9BFFF2752B120C93", "0x354AE49AAD8594E5"),
    (58, "0x1CCE2B95DE62FE75", "0xFD820DB65273DCF7"),
    (59, "0xC212713C2819087D", "0x251D066D30F10DF2"),
    (60, "0x3B0F69CE09DA2648", "0xC023ABBF5916189D"),
    (61, "0x5FA0418EADFA3AEE", "0x2758BD799E28BE90"),
    (62, "0x0A836A907A80935B", "0x5E17AB40E675696A"),
    (63, "0x10DD94885C71894A", "0x627EE118A072EC8A"),
    (64, "0x90083DA9CDB9D795", "0x13886553E7DC3FA3"),
    (65, "0x38824DEA10BF4E5D", "0x1C6516CC715D0A0E"),
    (72, "0xC05B604B7ED5B003", "0x6D4139641ACBAB81"),
    (100, "0xD248BFC5208B0B16", "0x985C0AA35F523FE6"),
    (127, "0xD2E4C27BD546BBA7", "0xC1A7DB96CF0DF2F2"),
    (128, "0x85CBE89FE5A0317D", "0xC4399C7829D0628F"),
    (129, "0x6C9A4008B493E757", "0x8433489056750B32"),
    (136, "0x1223F7EC6D27DF2C", "0xF2F387987B1CC934"),
    (240, "0xBBBBB56821328CAC", "0x3C0BB96864E543A1"),
    (241, "0x6C9698E4EFDC2E1F", "0xBFF7215089202D8F"),
    (255, "0x5DA8139D7ACF3995", "0x1729EAA78DF50A51"),
    (256, "0xB451505592B94D00", "0xC03402B5DCE3DBFA"),
    (512, "0x4856239B1E7F8962", "0x3743117A26EA4D71"),
    (1000, "0x6BE03ACBF959C413", "0xB8B406A98622D913"),
    (4096, "0xD39D19664A8B8D6A", "0x99E3C931DAB3E711"),
    (65536, "0x99F6248463CE0A2F", "0xCAFA8D1474483C22"),
]


class TestXxHash(unittest.TestCase):
    def test_vectors_against_bundled_xxhash(self):
        """Every length hits a different XXH3 code path (<=16, 17..128, 129..240, >240)."""
        for n, h6, h3 in VECTORS:
            data = pattern(n)
            self.assertEqual("%016X" % R.xxh64(data, 0), h6[2:], "xxh64 len %d" % n)
            self.assertEqual("%016X" % R.xxh3_64bits(data), h3[2:], "xxh3 len %d" % n)

    def test_empty_input_public_values(self):
        # the values published for xxHash 0.8.0
        self.assertEqual(R.xxh64(b"", 0), 0xEF46DB3751D8E999)
        self.assertEqual(R.xxh3_64bits(b""), 0x2D06800538D394C2)

    def test_seed_matters_for_xxh64(self):
        self.assertNotEqual(R.xxh64(b"rhyremix", 0), R.xxh64(b"rhyremix", 1))

    def test_avalanche_constant(self):
        """XXH3_avalanche in 0.8.0 multiplies by 0x165667919E3779F9, not XXH_PRIME64_3.

        A wrong constant here still hashes beautifully, it just gives hashes the game never
        produced - the easiest way to get this whole tool wrong, hence the explicit check.
        """
        k = 0x165667919E3779F9
        self.assertEqual(R._avalanche64(1), k ^ (k >> 32))
        self.assertNotEqual(k, R.P64_3)


class TestTxdParsing(unittest.TestCase):
    """A .txd built in memory, laid out exactly like the ones in models/."""

    @staticmethod
    def texture(name, w, h, depth, raster, fourcc=b"", name_chunk=0x09, size_field=None):
        pre = bytearray(32)
        if fourcc:
            pre[4:8] = fourcc
        head = struct.pack("<IIHHBBBB", 0, 0, w, h, depth, 1, 4, 0)
        head += struct.pack("<I", len(raster) if size_field is None else size_field)
        body = name.encode().ljust(32, b"\0") + bytes(pre) + head + raster
        inner = struct.pack("<II", name_chunk, len(body)) + body
        native = struct.pack("<III", 0x01, len(inner) + 4, 0x1803FFFF) + inner + b"\0\0\0\0"
        return struct.pack("<III", 0x15, len(native), 0x1803FFFF) + native

    def txd(self, textures):
        count = struct.pack("<III", 0x01, 4, 0x1803FFFF) + struct.pack("<I", len(textures))
        body = count + b"".join(textures)
        return struct.pack("<III", 0x16, len(body), 0x1803FFFF) + body

    def test_32bit_raster(self):
        rgba = bytes(range(4 * 4 * 4))          # 4x4 A8R8G8B8, 64 bytes
        data = self.txd([self.texture("mirror", 4, 4, 32, rgba)])
        texes = R.parse_txd(data, "mem")
        self.assertEqual(len(texes), 1)
        t = texes[0]
        self.assertEqual((t.name, t.w, t.h, t.depth, t.size), ("mirror", 4, 4, 32, 64))
        self.assertEqual(t.format_name(), "A8R8G8B8")
        self.assertEqual(t.d3d9_mip0(data), rgba)
        self.assertEqual(R.xxh3_64bits(t.d3d9_mip0(data)), R.xxh3_64bits(rgba))

    def test_16bit_raster_is_stored_one_byte_per_pixel(self):
        """SA's depth-16 rasters hold width*height bytes; that is what Remix hashes."""
        raster = bytes(range(4 * 4))            # 4x4 -> 16 bytes, not 32
        blob = self.txd([self.texture("font", 4, 4, 16, raster)])
        t = R.parse_txd(blob, "mem")[0]
        self.assertEqual((t.depth, t.size), (16, 16))
        self.assertEqual(t.d3d9_mip0(blob), raster)

    def test_dxt5_raster(self):
        blocks = bytes(range(64))               # 8x8 DXT5 = 2x2 blocks x 16 bytes
        data = self.txd([self.texture("dxt", 8, 8, 16, blocks, fourcc=b"DXT5")])
        t = R.parse_txd(data, "mem")[0]
        self.assertEqual((t.fourcc, t.format_name(), t.size), (b"DXT5", "DXT5", 64))
        self.assertEqual(t.d3d9_mip0(data), blocks)

    def test_old_name_chunk_type_8(self):
        rgba = b"\x11" * 64
        data = self.txd([self.texture("old", 4, 4, 32, rgba, name_chunk=0x08)])
        self.assertEqual(R.parse_txd(data, "mem")[0].name, "old")

    def test_row_padding_to_4_bytes(self):
        """GetMipSize aligns every row to 4 bytes; a 2x2 8-bit raster needs it."""
        raster = b"\x01\x02\x03\x04"
        blob = self.txd([self.texture("pad", 2, 2, 8, raster)])
        t = R.parse_txd(blob, "mem")[0]
        self.assertEqual((t.w, t.h, t.size), (2, 2, 4))
        self.assertEqual(t.d3d9_mip0(blob), b"\x01\x02\0\0\x03\x04\0\0")

    def test_bogus_size_word_is_refused_not_guessed(self):
        """models/txd/outro.txd: no size word -> say so instead of inventing a hash."""
        data = self.txd([self.texture("outro", 512, 512, 8, b"\0" * 64, size_field=0xFF000000)])
        t = R.parse_txd(data, "mem")[0]
        self.assertTrue(t.note)
        self.assertIn("no usable size word", t.note)

    def test_not_a_txd(self):
        with self.assertRaises(ValueError):
            R.parse_txd(b"VER2" + b"\0" * 64, "mem")

    def test_multiple_textures_and_names(self):
        a, b = b"A" * 16, b"B" * 16
        data = self.txd([self.texture("one", 2, 2, 32, a), self.texture("two", 2, 2, 32, b)])
        texes = R.parse_txd(data, "mem")
        self.assertEqual([t.name for t in texes], ["one", "two"])
        self.assertEqual([t.d3d9_mip0(data) for t in texes], [a, b])

    def test_img_archive_roundtrip(self):
        """A .txd inside a VER2 .img is found and labelled archive:entry."""
        blob = self.txd([self.texture("inimg", 2, 2, 32, b"\x7f" * 16)])
        blob += b"\0" * (-len(blob) % 2048)     # .img entries are whole 2048-byte sectors
        entry = struct.pack("<IHH24s", 1, len(blob) // 2048, 0, b"inimg.txd")
        head = b"VER2" + struct.pack("<I", 1) + entry
        img = head + b"\0" * (2048 - len(head)) + blob
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "gta3.img")
            open(p, "wb").write(img)
            got = list(R.iter_txd_bytes(p))
        self.assertEqual(len(got), 1)
        label, data = got[0]
        self.assertTrue(label.endswith(":inimg.txd"), label)
        self.assertEqual(R.parse_txd(data, label)[0].name, "inimg")


class TestRtxConf(unittest.TestCase):
    def test_reads_hash_lists(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "rtx.conf")
            open(p, "w").write(
                "rtx.particleTextures = 0x2F580E4763FAB662, 0x05D81D806AE2DE92\n"
                "rtx.animatedWaterTextures = 0x59296793798A7318,\n    0xDEADBEEFDEADBEEF\n"
                "rtx.uiTextures =\n"
                "rtx.notAHashList = 1\n"
                "rtx.someFloat = 0.5\n")
            conf = R.read_conf(p)
        self.assertEqual(sorted(conf), ["05D81D806AE2DE92", "2F580E4763FAB662",
                                       "59296793798A7318", "DEADBEEFDEADBEEF"])
        self.assertEqual(conf["59296793798A7318"], ["rtx.animatedWaterTextures"])
        self.assertEqual(conf["DEADBEEFDEADBEEF"], ["rtx.animatedWaterTextures"])

    def test_cli_reports_a_match(self):
        """tools/remix_texhash.py TXD NAME --hash ... prints the hash and marks it."""
        import contextlib
        import io

        rgba = bytes(range(64))
        blob = TestTxdParsing().txd([TestTxdParsing.texture("mirror", 4, 4, 32, rgba)])
        want = "%016X" % R.xxh3_64bits(rgba)
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "x.txd")
            open(p, "wb").write(blob)
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = R.main([p, "mirror", "--hash", "0x" + want])
            out = buf.getvalue()
        self.assertEqual(rc, 0)
        self.assertIn(want, out)
        self.assertIn("<== --hash", out)

    def test_cli_named_texture_prints_without_any_filter(self):
        """`tool file.txd name` must print the hash even with no --hash/--conf given."""
        import contextlib
        import io

        rgba = bytes(range(64))
        blob = TestTxdParsing().txd([TestTxdParsing.texture("mirror", 4, 4, 32, rgba)])
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "x.txd")
            open(p, "wb").write(blob)
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = R.main([p, "mirror"])
            out = buf.getvalue()
        self.assertEqual(rc, 0)
        self.assertIn("%016X" % R.xxh3_64bits(rgba), out)

    def test_cli_missing_name_fails(self):
        import contextlib
        import io

        blob = TestTxdParsing().txd([TestTxdParsing.texture("one", 2, 2, 32, b"A" * 16)])
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "x.txd")
            open(p, "wb").write(blob)
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = R.main([p, "nosuch"])
            out = buf.getvalue()
        self.assertEqual(rc, 1)
        self.assertIn("not found: nosuch", out)


class TestAgainstTheGame(unittest.TestCase):
    """The hashes the game itself produced (rtx.conf) and the ones s05 needs.

    Needs the (read-only) GTA SA RTX installation; skipped elsewhere. This is the part of the
    s18 gate that ties the tool to reality - see the table in
    .strata/notes-s18-remix-texture-hash.md for the full 55/167 result.
    """

    # (txd inside the game, texture, w, h, expected xxh3, rtx.conf list it must appear in)
    KNOWN = [
        ("models/particle.txd", "waterclear256", 128, 128, "59296793798A7318", "rtx.animatedWaterTextures"),
        ("models/fonts.txd", "font1", 512, 512, "3FC78C823061BDB0", "rtx.uiTextures"),
        ("models/fonts.txd", "font2", 512, 512, "5970DA3808FA8128", "rtx.uiTextures"),
        ("models/hud.txd", "siteM16", 64, 64, "C412E7D486FF9E5C", "rtx.uiTextures"),
        ("models/particle.txd", "headlight", 64, 64, "B6E06CE024FE9111", "rtx.lightmapTextures"),
        ("models/particle.txd", "shad_car", 64, 64, "8F34BF39FA23F39D", "rtx.ignoreTextures"),
        ("models/particle.txd", "particleskid", 32, 32, "66EFA5FE769C8102", "rtx.decalTextures"),
        ("models/effectsPC.txd", "sphere", 64, 64, "2F580E4763FAB662", "rtx.particleTextures"),
        ("models/generic/vehicle.txd", "vehicledash32", 32, 32, "CBFFF905D20D2591", None),
        ("models/generic/vehicle.txd", "xvehicleenv128", 128, 128, "45DF1EA60BB202B5", None),
        ("models/generic/vehicle.txd", "vehicleenvmap128", 128, 128, "6BAFC5F6AEE0A6B5", None),
    ]

    def setUp(self):
        if not os.path.isdir(GAME):
            self.skipTest("%s not installed" % GAME)

    def test_hashes_and_sizes(self):
        conf = R.read_conf(os.path.join(GAME, "rtx.conf"))
        for rel, name, w, h, want, key in self.KNOWN:
            path = os.path.join(GAME, rel)
            blob = open(path, "rb").read()
            hits = [t for t in R.parse_txd(blob, path) if t.name.lower() == name.lower()]
            self.assertEqual(len(hits), 1, "%s in %s" % (name, rel))
            t = hits[0]
            self.assertEqual((t.w, t.h), (w, h), "%s size" % name)
            self.assertEqual("%016X" % R.xxh3_64bits(t.d3d9_mip0(blob)), want, name)
            if key:
                self.assertIn(want, conf, "%s is not in rtx.conf at all" % name)
                self.assertIn(key, conf[want], "%s: expected %s, got %s" % (name, key, conf[want]))

    def test_mirror_hashes_match_the_config_file(self):
        """config/mirror-hashes.txt must carry exactly what the game's .txd produces."""
        cfg = os.path.join(os.path.dirname(__file__), "..", "config", "mirror-hashes.txt")
        lines = [l.strip() for l in open(cfg, encoding="utf-8") if l.strip() and not l.startswith("#")]
        live = [l.split("#")[0].strip() for l in lines if not l.startswith("#")]
        self.assertEqual(sorted(live), ["0x45DF1EA60BB202B5", "0x6BAFC5F6AEE0A6B5"],
                         "config/mirror-hashes.txt has no (or wrong) live hashes")
        blob = open(os.path.join(GAME, "models/generic/vehicle.txd"), "rb").read()
        by_name = {t.name.lower(): t for t in R.parse_txd(blob, "vehicle.txd")}
        for name in ("xvehicleenv128", "vehicleenvmap128"):
            want = "%016X" % R.xxh3_64bits(by_name[name].d3d9_mip0(blob))
            self.assertIn("0x" + want, live, "%s hash in the config is not the game's" % name)


if __name__ == "__main__":
    unittest.main()
