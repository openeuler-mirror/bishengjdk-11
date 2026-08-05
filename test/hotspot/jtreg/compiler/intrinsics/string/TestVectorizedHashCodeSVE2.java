/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this work; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

/**
 * @test
 * @summary Validate the AArch64 SVE2 ArraysSupport.vectorizedHashCode intrinsic path.
 * @requires os.arch == "aarch64" & vm.compiler2.enabled & vm.cpu.features ~= ".*sve2.*"
 * @requires vm.flagless
 * @library /test/lib /
 * @modules java.base/jdk.internal.misc
 *          java.base/jdk.internal.util
 *          java.management
 *
 * @build sun.hotspot.WhiteBox
 * @build compiler.intrinsics.string.TestVectorizedHashCodeSVE2
 * @run driver ClassFileInstaller sun.hotspot.WhiteBox
 * @run main compiler.intrinsics.string.TestVectorizedHashCodeSVE2
 */

package compiler.intrinsics.string;

import jtreg.SkippedException;

import java.lang.reflect.Method;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import jdk.internal.util.ArraysSupport;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;
import sun.hotspot.WhiteBox;

public class TestVectorizedHashCodeSVE2 {
    private static final int COMP_LEVEL_FULL_OPTIMIZATION = 4;
    private static final int WARMUP_ROUNDS = 1_000;
    private static final long DEFAULT_MIN_CHUNKS = 1;
    private static final long EXPLICIT_MIN_CHUNKS = 2;
    private static final long LATIN1_MIN_ELEMENTS = 64;
    private static final long BYTE_MIN_ELEMENTS = 64;
    private static final long UTF16_MIN_ELEMENTS = 256;
    private static final long SHORT_MIN_ELEMENTS = 256;
    private static final int[] LATIN1_LENGTHS = {
            32, 63, 64, 65, 96, 127, 128, 4096
    };
    private static final int[] UTF16_LENGTHS = {
            16, 31, 32, 33, 63, 64, 65, 4096
    };
    private static final int[] BYTE_LENGTHS = LATIN1_LENGTHS;
    private static final int[] SHORT_LENGTHS = UTF16_LENGTHS;
    private static final int[] INT_LENGTHS = {
            8, 15, 16, 17, 31, 32, 33, 63, 64, 65, 4096
    };
    private static final int[] OFFSETS = {0, 1, 3, 7};
    private static final int[] INITIAL_VALUES = {0, 1, 0x12345678, 0x89abcdef};
    private static final boolean BIG_ENDIAN =
            ByteOrder.nativeOrder() == ByteOrder.BIG_ENDIAN;

    private static final byte[] LATIN1 = new byte[max(LATIN1_LENGTHS) + max(OFFSETS) + 32];
    private static final byte[] SIGNED_BYTES =
            new byte[max(BYTE_LENGTHS) + max(OFFSETS) + 32];
    private static final byte[] UTF16_BYTES =
            new byte[(max(UTF16_LENGTHS) + max(OFFSETS) + 32) * 2];
    private static final char[] UTF16_CHARS =
            new char[max(UTF16_LENGTHS) + max(OFFSETS) + 32];
    private static final short[] SIGNED_SHORTS =
            new short[max(SHORT_LENGTHS) + max(OFFSETS) + 32];
    private static final int[] SIGNED_INTS =
            new int[max(INT_LENGTHS) + max(OFFSETS) + 32];

    private static final String CLASS_NAME = TestVectorizedHashCodeSVE2.class.getName();
    private static final String RUNNER_ARG = "runner";
    private static final String PROBE_ARG = "probe";
    private static final String PASS_MESSAGE =
            "SVE2 vectorizedHashCode intrinsic path test passed";
    private static final Pattern HISILICON_FEATURES_PATTERN = Pattern.compile("(?m)^0x48:");
    private static final Pattern SVE2_STUB_PATTERN = Pattern.compile(
            "(StubRoutines::_large_arrays_hashcode_sve2_(boolean|byte|char|short)) " +
            "\\[(0x[0-9a-fA-F]+), (0x[0-9a-fA-F]+)\\[");
    private static final Pattern CODE_LINE_PATTERN = Pattern.compile(
            "^\\s*(0x[0-9a-fA-F]+):\\s*(.*)$");
    private static final Pattern HEX_HALFWORD_PATTERN = Pattern.compile("\\b[0-9a-fA-F]{4}\\b");

    public static void main(String[] args) throws Exception {
        if (args.length >= 1 && RUNNER_ARG.equals(args[0])) {
            runInTestVM(args);
        } else if (args.length >= 1 && PROBE_ARG.equals(args[0])) {
            runProbe();
        } else {
            runDriver();
        }
    }

    private static void runDriver() throws Exception {
        String features = probeCPUFeatures();
        if (!isHiSiliconCPU(features)) {
            throw new SkippedException("SVE2 hashCode intrinsic is only enabled on HiSilicon CPUs: "
                    + features);
        }

        runDriverCase("default-thresholds", DEFAULT_MIN_CHUNKS);
        runDriverCase("explicit-thresholds", EXPLICIT_MIN_CHUNKS,
                "-XX:SVEHashCodeStubMinVectorChunks=" + EXPLICIT_MIN_CHUNKS,
                "-XX:SVEHashCodeLatin1MinElements=" + LATIN1_MIN_ELEMENTS,
                "-XX:SVEHashCodeByteMinElements=" + BYTE_MIN_ELEMENTS,
                "-XX:SVEHashCodeUTF16MinElements=" + UTF16_MIN_ELEMENTS,
                "-XX:SVEHashCodeShortMinElements=" + SHORT_MIN_ELEMENTS);
    }

    private static void runDriverCase(String testCase, long expectedMinChunks,
                                      String... extraFlags) throws Exception {
        List<String> command = new ArrayList<>();
        command.add("--add-exports");
        command.add("java.base/jdk.internal.util=ALL-UNNAMED");
        command.add("-Xbootclasspath/a:.");
        command.add("-Xbatch");
        command.add("-XX:-TieredCompilation");
        command.add("-XX:CompileThreshold=100");
        command.add("-XX:+UnlockDiagnosticVMOptions");
        command.add("-XX:+WhiteBoxAPI");
        command.add("-XX:+UseVectorizedHashCodeIntrinsic");
        command.add("-XX:+UseSVEHashCodeIntrinsic");
        command.add("-XX:UseSVE=2");
        for (String flag : extraFlags) {
            command.add(flag);
        }
        command.add("-XX:+PrintStubCode");
        command.add("-XX:+PrintAssembly");
        command.add("-XX:CompileCommand=quiet");
        command.add("-XX:CompileCommand=compileonly," + CLASS_NAME + "::vector*");
        command.add("-XX:CompileCommand=print," + CLASS_NAME + "::vector*");
        command.add(CLASS_NAME);
        command.add(RUNNER_ARG);
        command.add(testCase);
        command.add(Long.toString(expectedMinChunks));

        ProcessBuilder pb = ProcessTools.createTestJvm(
                command.toArray(new String[0]));

        OutputAnalyzer output = ProcessTools.executeProcess(pb);
        output.shouldHaveExitValue(0);

        String text = output.getOutput();
        if (!text.contains(passMessage(testCase))) {
            output.reportDiagnosticSummary();
            throw new AssertionError("The runner did not report successful validation");
        }

        StubRange latin1Stub = findSVE2Stub(text, "boolean");
        StubRange byteStub = findSVE2Stub(text, "byte");
        StubRange utf16Stub = findSVE2Stub(text, "char");
        StubRange shortStub = findSVE2Stub(text, "short");

        checkCallsStub(text, "vectorLatin1", latin1Stub);
        checkCallsStub(text, "vectorByte", byteStub);
        checkCallsStub(text, "vectorUtf16Byte", utf16Stub);
        checkCallsStub(text, "vectorUtf16Char", utf16Stub);
        checkCallsStub(text, "vectorShort", shortStub);
        checkAbsent(text, "StubRoutines::large_arrays_hashcode_sve_boolean",
                "SVE1 hashcode stubs should not be generated");
        checkAbsent(text, "StubRoutines::large_arrays_hashcode_sve_char",
                "SVE1 hashcode stubs should not be generated");
        checkAbsent(text, "StubRoutines::large_arrays_hashcode_sve_int",
                "T_INT must stay on the base NEON hashcode stub");
    }

    private static String probeCPUFeatures() throws Exception {
        ProcessBuilder pb = ProcessTools.createTestJvm(
                "-Xbootclasspath/a:.",
                "-XX:+UnlockDiagnosticVMOptions",
                "-XX:+WhiteBoxAPI",
                CLASS_NAME,
                PROBE_ARG);
        OutputAnalyzer output = ProcessTools.executeProcess(pb);
        output.shouldHaveExitValue(0);
        return output.getOutput().trim();
    }

    private static void runProbe() {
        System.out.println(WhiteBox.getWhiteBox().getCPUFeatures());
    }

    private static boolean isHiSiliconCPU(String features) {
        return features != null && HISILICON_FEATURES_PATTERN.matcher(features).find();
    }

    private static String passMessage(String testCase) {
        return PASS_MESSAGE + ": " + testCase;
    }

    private static StubRange findSVE2Stub(String output, String stubType) {
        return findStub(output, stubType, SVE2_STUB_PATTERN, "SVE2");
    }

    private static StubRange findStub(String output, String stubType, Pattern pattern,
                                      String stubKind) {
        Matcher matcher = pattern.matcher(output);
        while (matcher.find()) {
            if (stubType.equals(matcher.group(2))) {
                return new StubRange(matcher.group(1),
                        parseAddress(matcher.group(3)), parseAddress(matcher.group(4)));
            }
        }
        throw new AssertionError("Could not find " + stubKind + " " + stubType
                + " hashCode stub in output");
    }

    private static void checkCallsStub(String output, String methodName, StubRange stub) {
        String block = findC2AssemblyBlock(output, methodName);

        // Prefer hsdis' symbolic call target when present; older or different
        // disassemblers may only leave the raw BL instruction bytes.
        if (block.contains(stub.name)) {
            return;
        }

        List<Long> targets = findDirectBranchLinkTargets(block);
        for (long target : targets) {
            if (stub.contains(target)) {
                return;
            }
        }
        throw new AssertionError(methodName + " does not call " + stub.name
                + "; direct BL targets=" + formatTargets(targets));
    }

    private static void checkAbsent(String output, String needle, String message) {
        if (output.contains(needle)) {
            throw new AssertionError(message + ": found " + needle);
        }
    }

    private static String findC2AssemblyBlock(String output, String methodName) {
        Pattern pattern = Pattern.compile("(?s)Compiled method \\(c2\\).*?" +
                Pattern.quote(CLASS_NAME + "::" + methodName) + ".*?\\[/MachCode\\]");
        Matcher matcher = pattern.matcher(output);
        if (matcher.find()) {
            return matcher.group();
        }
        throw new AssertionError("Could not find C2 assembly for " + methodName);
    }

    private static List<Long> findDirectBranchLinkTargets(String block) {
        List<Long> targets = new ArrayList<>();
        String[] lines = block.split("\\R");
        for (String line : lines) {
            Matcher lineMatcher = CODE_LINE_PATTERN.matcher(line);
            if (!lineMatcher.find()) {
                continue;
            }

            long pc = parseAddress(lineMatcher.group(1));
            Matcher halfwords = HEX_HALFWORD_PATTERN.matcher(lineMatcher.group(2));
            String pending = null;
            while (halfwords.find()) {
                if (pending == null) {
                    pending = halfwords.group();
                    continue;
                }
                int word = decodeAArch64Word(pending, halfwords.group());
                long unsignedWord = Integer.toUnsignedLong(word);
                if ((unsignedWord & 0xfc00_0000L) == 0x9400_0000L) {
                    long imm26 = unsignedWord & 0x03ff_ffffL;
                    if ((imm26 & 0x0200_0000L) != 0) {
                        imm26 |= ~0x03ff_ffffL;
                    }
                    targets.add(pc + (imm26 << 2));
                }
                pc += 4;
                pending = null;
            }
        }
        return targets;
    }

    private static int decodeAArch64Word(String lowHalfword, String highHalfword) {
        int b0 = Integer.parseInt(lowHalfword.substring(0, 2), 16);
        int b1 = Integer.parseInt(lowHalfword.substring(2, 4), 16);
        int b2 = Integer.parseInt(highHalfword.substring(0, 2), 16);
        int b3 = Integer.parseInt(highHalfword.substring(2, 4), 16);
        return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
    }

    private static String formatTargets(List<Long> targets) {
        if (targets.isEmpty()) {
            return "[]";
        }

        StringBuilder result = new StringBuilder("[");
        for (int i = 0; i < targets.size(); i++) {
            if (i > 0) {
                result.append(", ");
            }
            result.append("0x").append(Long.toHexString(targets.get(i)));
        }
        return result.append("]").toString();
    }

    private static long parseAddress(String address) {
        return Long.parseUnsignedLong(address.substring(2).toLowerCase(Locale.ROOT), 16);
    }

    private static void runInTestVM(String[] args) throws Exception {
        if (args.length != 3) {
            throw new AssertionError("Expected runner arguments: "
                    + RUNNER_ARG + " <test-case> <expected-min-chunks>");
        }

        String testCase = args[1];
        long expectedMinChunks = Long.parseLong(args[2]);
        WhiteBox whiteBox = WhiteBox.getWhiteBox();

        requireActualSVE2(whiteBox, expectedMinChunks);
        requireHashCodeIntrinsic(whiteBox);
        initializeInputs();

        Method latin1 = method("vectorLatin1", byte[].class, int.class, int.class, int.class);
        Method signedByte = method("vectorByte", byte[].class, int.class, int.class, int.class);
        Method utf16Byte = method("vectorUtf16Byte", byte[].class, int.class, int.class, int.class);
        Method utf16Char = method("vectorUtf16Char", char[].class, int.class, int.class, int.class);
        Method signedShort = method("vectorShort", short[].class, int.class, int.class, int.class);
        Method signedInt = method("vectorInt", int[].class, int.class, int.class, int.class);

        whiteBox.testSetDontInlineMethod(latin1, true);
        whiteBox.testSetDontInlineMethod(signedByte, true);
        whiteBox.testSetDontInlineMethod(utf16Byte, true);
        whiteBox.testSetDontInlineMethod(utf16Char, true);
        whiteBox.testSetDontInlineMethod(signedShort, true);
        whiteBox.testSetDontInlineMethod(signedInt, true);

        for (int round = 0; round < WARMUP_ROUNDS; round++) {
            checkAll();
        }

        compileAndCheck(whiteBox, latin1);
        compileAndCheck(whiteBox, signedByte);
        compileAndCheck(whiteBox, utf16Byte);
        compileAndCheck(whiteBox, utf16Char);
        compileAndCheck(whiteBox, signedShort);
        compileAndCheck(whiteBox, signedInt);

        checkAll();
        System.out.println(passMessage(testCase));
    }

    private static void requireActualSVE2(WhiteBox whiteBox, long expectedMinChunks)
            throws Exception {
        String features = whiteBox.getCPUFeatures();
        if (features == null || !features.contains("sve2")) {
            throw new AssertionError("SVE2 CPU feature is not available: " + features);
        }
        if (!isHiSiliconCPU(features)) {
            throw new AssertionError("Expected HiSilicon CPU for SVE2 hashCode intrinsic: "
                    + features);
        }

        long useSVE = whiteBox.getUintVMFlag("UseSVE");
        if (useSVE != 2) {
            throw new AssertionError("Expected actual UseSVE=2, got " + useSVE);
        }

        long minChunks = whiteBox.getUintVMFlag("SVEHashCodeStubMinVectorChunks");
        if (minChunks != expectedMinChunks) {
            throw new AssertionError("Expected SVEHashCodeStubMinVectorChunks="
                    + expectedMinChunks + ", got " + minChunks);
        }

        checkUintFlag(whiteBox, "SVEHashCodeLatin1MinElements", LATIN1_MIN_ELEMENTS);
        checkUintFlag(whiteBox, "SVEHashCodeByteMinElements", BYTE_MIN_ELEMENTS);
        checkUintFlag(whiteBox, "SVEHashCodeUTF16MinElements", UTF16_MIN_ELEMENTS);
        checkUintFlag(whiteBox, "SVEHashCodeShortMinElements", SHORT_MIN_ELEMENTS);

        Boolean useSVEHashCode = whiteBox.getBooleanVMFlag("UseSVEHashCodeIntrinsic");
        if (!Boolean.TRUE.equals(useSVEHashCode)) {
            throw new AssertionError("UseSVEHashCodeIntrinsic is not enabled");
        }
        Boolean useVectorizedHashCode = whiteBox.getBooleanVMFlag("UseVectorizedHashCodeIntrinsic");
        if (!Boolean.TRUE.equals(useVectorizedHashCode)) {
            throw new AssertionError("UseVectorizedHashCodeIntrinsic is not enabled");
        }
    }

    private static void checkUintFlag(WhiteBox whiteBox, String name, long expected) {
        long actual = whiteBox.getUintVMFlag(name);
        if (actual != expected) {
            throw new AssertionError("Expected " + name + "=" + expected + ", got " + actual);
        }
    }

    private static void requireHashCodeIntrinsic(WhiteBox whiteBox) throws Exception {
        Method intrinsic = ArraysSupport.class.getDeclaredMethod("vectorizedHashCode",
                Object.class, int.class, int.class, int.class, int.class);
        if (!whiteBox.isIntrinsicAvailable(intrinsic, COMP_LEVEL_FULL_OPTIMIZATION)) {
            throw new AssertionError("ArraysSupport.vectorizedHashCode intrinsic is unavailable");
        }
    }

    private static Method method(String name, Class<?>... parameterTypes) throws Exception {
        Method method = TestVectorizedHashCodeSVE2.class.getDeclaredMethod(name, parameterTypes);
        method.setAccessible(true);
        return method;
    }

    private static void compileAndCheck(WhiteBox whiteBox, Method method) throws Exception {
        if (!whiteBox.enqueueMethodForCompilation(method, COMP_LEVEL_FULL_OPTIMIZATION)) {
            throw new AssertionError("Failed to enqueue " + method + " for C2 compilation");
        }
        for (int i = 0; i < 100 && !whiteBox.isMethodCompiled(method); i++) {
            Thread.sleep(10);
        }
        if (!whiteBox.isMethodCompiled(method)) {
            throw new AssertionError(method + " was not compiled");
        }
        int level = whiteBox.getMethodCompilationLevel(method);
        if (level != COMP_LEVEL_FULL_OPTIMIZATION) {
            throw new AssertionError(method + " compiled at level " + level
                    + ", expected " + COMP_LEVEL_FULL_OPTIMIZATION);
        }
    }

    private static void initializeInputs() {
        for (int i = 0; i < LATIN1.length; i++) {
            LATIN1[i] = (byte) ((i * 131 + 17) ^ (i >>> 1));
        }
        for (int i = 0; i < SIGNED_BYTES.length; i++) {
            SIGNED_BYTES[i] = (byte) ((i * 29 - 128) ^ (i >>> 2));
        }
        for (int i = 0; i < UTF16_CHARS.length; i++) {
            char c = (char) ((i * 769 + 0x4321) ^ (i << 5));
            UTF16_CHARS[i] = c;
            putChar(UTF16_BYTES, i, c);
        }
        for (int i = 0; i < SIGNED_SHORTS.length; i++) {
            SIGNED_SHORTS[i] = (short) ((i * 9973 - 0x4000) ^ (i << 3));
        }
        for (int i = 0; i < SIGNED_INTS.length; i++) {
            SIGNED_INTS[i] = (i * 0x9e3779b9) ^ (i << 13) ^ (i >>> 7);
        }
    }

    private static void checkAll() {
        for (int offset : OFFSETS) {
            for (int initialValue : INITIAL_VALUES) {
                for (int length : LATIN1_LENGTHS) {
                    int expected = latin1Hash(LATIN1, offset, length, initialValue);
                    int actual = vectorLatin1(LATIN1, offset, length, initialValue);
                    check("latin1", offset, length, initialValue, expected, actual);
                }
                for (int length : BYTE_LENGTHS) {
                    int expected = byteHash(SIGNED_BYTES, offset, length, initialValue);
                    int actual = vectorByte(SIGNED_BYTES, offset, length, initialValue);
                    check("byte", offset, length, initialValue, expected, actual);
                }
                for (int length : UTF16_LENGTHS) {
                    int expected = utf16ByteHash(UTF16_BYTES, offset, length, initialValue);
                    int actual = vectorUtf16Byte(UTF16_BYTES, offset, length, initialValue);
                    check("utf16-byte", offset, length, initialValue, expected, actual);

                    expected = utf16CharHash(UTF16_CHARS, offset, length, initialValue);
                    actual = vectorUtf16Char(UTF16_CHARS, offset, length, initialValue);
                    check("utf16-char", offset, length, initialValue, expected, actual);
                }
                for (int length : SHORT_LENGTHS) {
                    int expected = shortHash(SIGNED_SHORTS, offset, length, initialValue);
                    int actual = vectorShort(SIGNED_SHORTS, offset, length, initialValue);
                    check("short", offset, length, initialValue, expected, actual);
                }
                for (int length : INT_LENGTHS) {
                    int expected = intHash(SIGNED_INTS, offset, length, initialValue);
                    int actual = vectorInt(SIGNED_INTS, offset, length, initialValue);
                    check("int", offset, length, initialValue, expected, actual);
                }
            }
        }
    }

    private static int vectorLatin1(byte[] value, int offset, int length, int initialValue) {
        return ArraysSupport.vectorizedHashCode(value, offset, length, initialValue,
                ArraysSupport.T_BOOLEAN);
    }

    private static int vectorByte(byte[] value, int offset, int length, int initialValue) {
        return ArraysSupport.vectorizedHashCode(value, offset, length, initialValue,
                ArraysSupport.T_BYTE);
    }

    private static int vectorUtf16Byte(byte[] value, int offset, int length, int initialValue) {
        return ArraysSupport.vectorizedHashCode(value, offset, length, initialValue,
                ArraysSupport.T_CHAR);
    }

    private static int vectorUtf16Char(char[] value, int offset, int length, int initialValue) {
        return ArraysSupport.vectorizedHashCode(value, offset, length, initialValue,
                ArraysSupport.T_CHAR);
    }

    private static int vectorShort(short[] value, int offset, int length, int initialValue) {
        return ArraysSupport.vectorizedHashCode(value, offset, length, initialValue,
                ArraysSupport.T_SHORT);
    }

    private static int vectorInt(int[] value, int offset, int length, int initialValue) {
        return ArraysSupport.vectorizedHashCode(value, offset, length, initialValue,
                ArraysSupport.T_INT);
    }

    private static int latin1Hash(byte[] value, int offset, int length, int result) {
        for (int i = offset; i < offset + length; i++) {
            result = 31 * result + (value[i] & 0xff);
        }
        return result;
    }

    private static int byteHash(byte[] value, int offset, int length, int result) {
        for (int i = offset; i < offset + length; i++) {
            result = 31 * result + value[i];
        }
        return result;
    }

    private static int utf16ByteHash(byte[] value, int offset, int length, int result) {
        for (int i = offset; i < offset + length; i++) {
            result = 31 * result + getChar(value, i);
        }
        return result;
    }

    private static int utf16CharHash(char[] value, int offset, int length, int result) {
        for (int i = offset; i < offset + length; i++) {
            result = 31 * result + value[i];
        }
        return result;
    }

    private static int shortHash(short[] value, int offset, int length, int result) {
        for (int i = offset; i < offset + length; i++) {
            result = 31 * result + value[i];
        }
        return result;
    }

    private static int intHash(int[] value, int offset, int length, int result) {
        for (int i = offset; i < offset + length; i++) {
            result = 31 * result + value[i];
        }
        return result;
    }

    private static void check(String name, int offset, int length, int initialValue,
                              int expected, int actual) {
        if (actual != expected) {
            throw new AssertionError(name + " mismatch: offset=" + offset
                    + ", length=" + length
                    + ", initialValue=" + initialValue
                    + ", expected=" + expected
                    + ", actual=" + actual);
        }
    }

    private static void putChar(byte[] value, int index, char c) {
        index <<= 1;
        if (BIG_ENDIAN) {
            value[index] = (byte) (c >>> 8);
            value[index + 1] = (byte) c;
        } else {
            value[index] = (byte) c;
            value[index + 1] = (byte) (c >>> 8);
        }
    }

    private static char getChar(byte[] value, int index) {
        index <<= 1;
        if (BIG_ENDIAN) {
            return (char) (((value[index] & 0xff) << 8) | (value[index + 1] & 0xff));
        }
        return (char) ((value[index] & 0xff) | ((value[index + 1] & 0xff) << 8));
    }

    private static int max(int[] values) {
        int result = values[0];
        for (int value : values) {
            result = Math.max(result, value);
        }
        return result;
    }

    private static class StubRange {
        private final String name;
        private final long begin;
        private final long end;

        private StubRange(String name, long begin, long end) {
            this.name = name;
            this.begin = begin;
            this.end = end;
        }

        private boolean contains(long pc) {
            return Long.compareUnsigned(pc, begin) >= 0 && Long.compareUnsigned(pc, end) < 0;
        }
    }
}
