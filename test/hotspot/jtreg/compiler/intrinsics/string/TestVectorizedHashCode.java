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
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

/**
 * @test
 * @summary Validate ArraysSupport.vectorizedHashCode with AArch64 SVE hashCode stubs.
 * @requires os.arch == "aarch64" & vm.compiler2.enabled
 * @modules java.base/jdk.internal.util
 * @requires vm.flagless
 *
 * @run main/othervm/timeout=240 -Xbatch -XX:-TieredCompilation -XX:CompileThreshold=100
 *      -XX:+UnlockDiagnosticVMOptions -XX:+UseVectorizedHashCodeIntrinsic
 *      -XX:+UseSVEHashCodeIntrinsic -XX:UseSVE=0
 *      -XX:CompileCommand=compileonly,compiler.intrinsics.string.TestVectorizedHashCode::vector*
 *      compiler.intrinsics.string.TestVectorizedHashCode
 *
 * @run main/othervm/timeout=240 -Xbatch -XX:-TieredCompilation -XX:CompileThreshold=100
 *      -XX:+UnlockDiagnosticVMOptions -XX:+UseVectorizedHashCodeIntrinsic
 *      -XX:+UseSVEHashCodeIntrinsic -XX:UseSVE=1
 *      -XX:CompileCommand=compileonly,compiler.intrinsics.string.TestVectorizedHashCode::vector*
 *      compiler.intrinsics.string.TestVectorizedHashCode
 *
 * @run main/othervm/timeout=240 -Xbatch -XX:-TieredCompilation -XX:CompileThreshold=100
 *      -XX:+UnlockDiagnosticVMOptions -XX:+UseVectorizedHashCodeIntrinsic
 *      -XX:+UseSVEHashCodeIntrinsic -XX:UseSVE=2
 *      -XX:CompileCommand=compileonly,compiler.intrinsics.string.TestVectorizedHashCode::vector*
 *      compiler.intrinsics.string.TestVectorizedHashCode
 *
 * @run main/othervm/timeout=240 -Xbatch -XX:-TieredCompilation -XX:CompileThreshold=100
 *      -XX:+UnlockDiagnosticVMOptions -XX:+UseVectorizedHashCodeIntrinsic
 *      -XX:-UseSVEHashCodeIntrinsic -XX:UseSVE=2
 *      -XX:CompileCommand=compileonly,compiler.intrinsics.string.TestVectorizedHashCode::vector*
 *      compiler.intrinsics.string.TestVectorizedHashCode
 */

package compiler.intrinsics.string;

import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;

import jdk.internal.util.ArraysSupport;

public class TestVectorizedHashCode {
    private static final int WARMUP_ROUNDS = 200;
    private static final int[] LENGTHS = {
            0, 1, 2, 3, 4, 7, 8, 15, 16, 17,
            31, 32, 33, 47, 48, 49, 63, 64, 65,
            95, 96, 97, 127, 128, 129, 255, 256, 257,
            511, 512, 513, 1023, 1024, 1025, 4096
    };
    private static final int[] OFFSETS = {0, 1, 2, 3, 5, 7, 11, 16};
    private static final int[] INITIAL_VALUES = {
            0, 1, -1, 31, 0x12345678, 0x89abcdef
    };
    private static final boolean BIG_ENDIAN =
            ByteOrder.nativeOrder() == ByteOrder.BIG_ENDIAN;

    public static void main(String[] args) {
        List<TestCase> cases = buildCases();

        for (int round = 0; round < WARMUP_ROUNDS; round++) {
            for (TestCase testCase : cases) {
                testCase.check();
            }
        }

        int checked = 0;
        for (TestCase testCase : cases) {
            testCase.check();
            checked++;
        }

        System.out.println("Test passed: " + checked + " vectorizedHashCode cases");
    }

    private static List<TestCase> buildCases() {
        List<TestCase> cases = new ArrayList<>();
        for (int length : LENGTHS) {
            for (int offset : OFFSETS) {
                for (int initialValue : INITIAL_VALUES) {
                    cases.add(latin1Case(offset, length, initialValue));
                    cases.add(byteArrayCase(offset, length, initialValue));
                    cases.add(utf16ByteCase(offset, length, initialValue));
                    cases.add(charArrayCase(offset, length, initialValue));
                    cases.add(shortArrayCase(offset, length, initialValue));
                    cases.add(intArrayCase(offset, length, initialValue));
                }
            }
        }
        return cases;
    }

    private static TestCase latin1Case(int offset, int length, int initialValue) {
        byte[] value = new byte[offset + length + 32];
        for (int i = 0; i < value.length; i++) {
            value[i] = (byte) ((i * 131 + 17) ^ (i >>> 1));
        }
        int expected = latin1Hash(value, offset, length, initialValue);
        return new TestCase("latin1", offset, length, initialValue, expected,
                () -> vectorLatin1(value, offset, length, initialValue));
    }

    private static TestCase byteArrayCase(int offset, int length, int initialValue) {
        byte[] value = new byte[offset + length + 32];
        for (int i = 0; i < value.length; i++) {
            value[i] = (byte) ((i * 29 - 128) ^ (i >>> 2));
        }
        int expected = byteArrayHash(value, offset, length, initialValue);
        return new TestCase("byte-array", offset, length, initialValue, expected,
                () -> vectorByteArray(value, offset, length, initialValue));
    }

    private static TestCase utf16ByteCase(int offset, int length, int initialValue) {
        byte[] value = new byte[(offset + length + 32) * 2];
        for (int i = 0; i < offset + length + 32; i++) {
            putChar(value, i, (char) ((i * 257 + 0x1234) ^ (i << 3)));
        }
        int expected = utf16ByteHash(value, offset, length, initialValue);
        return new TestCase("utf16-byte", offset, length, initialValue, expected,
                () -> vectorUtf16Byte(value, offset, length, initialValue));
    }

    private static TestCase charArrayCase(int offset, int length, int initialValue) {
        char[] value = new char[offset + length + 32];
        for (int i = 0; i < value.length; i++) {
            value[i] = (char) ((i * 769 + 0x4321) ^ (i << 5));
        }
        int expected = charArrayHash(value, offset, length, initialValue);
        return new TestCase("char-array", offset, length, initialValue, expected,
                () -> vectorCharArray(value, offset, length, initialValue));
    }

    private static TestCase shortArrayCase(int offset, int length, int initialValue) {
        short[] value = new short[offset + length + 32];
        for (int i = 0; i < value.length; i++) {
            value[i] = (short) ((i * 9973 - 0x4000) ^ (i << 3));
        }
        int expected = shortArrayHash(value, offset, length, initialValue);
        return new TestCase("short-array", offset, length, initialValue, expected,
                () -> vectorShortArray(value, offset, length, initialValue));
    }

    private static TestCase intArrayCase(int offset, int length, int initialValue) {
        int[] value = new int[offset + length + 32];
        for (int i = 0; i < value.length; i++) {
            value[i] = (i * 0x9e3779b9) ^ (i << 13) ^ (i >>> 7);
        }
        int expected = intArrayHash(value, offset, length, initialValue);
        return new TestCase("int-array", offset, length, initialValue, expected,
                () -> vectorIntArray(value, offset, length, initialValue));
    }

    private static int vectorLatin1(byte[] value, int offset, int length, int initialValue) {
        return ArraysSupport.vectorizedHashCode(value, offset, length, initialValue,
                ArraysSupport.T_BOOLEAN);
    }

    private static int vectorByteArray(byte[] value, int offset, int length, int initialValue) {
        return ArraysSupport.vectorizedHashCode(value, offset, length, initialValue,
                ArraysSupport.T_BYTE);
    }

    private static int vectorUtf16Byte(byte[] value, int offset, int length, int initialValue) {
        return ArraysSupport.vectorizedHashCode(value, offset, length, initialValue,
                ArraysSupport.T_CHAR);
    }

    private static int vectorCharArray(char[] value, int offset, int length, int initialValue) {
        return ArraysSupport.vectorizedHashCode(value, offset, length, initialValue,
                ArraysSupport.T_CHAR);
    }

    private static int vectorShortArray(short[] value, int offset, int length, int initialValue) {
        return ArraysSupport.vectorizedHashCode(value, offset, length, initialValue,
                ArraysSupport.T_SHORT);
    }

    private static int vectorIntArray(int[] value, int offset, int length, int initialValue) {
        return ArraysSupport.vectorizedHashCode(value, offset, length, initialValue,
                ArraysSupport.T_INT);
    }

    private static int latin1Hash(byte[] value, int offset, int length, int result) {
        for (int i = offset; i < offset + length; i++) {
            result = 31 * result + (value[i] & 0xff);
        }
        return result;
    }

    private static int byteArrayHash(byte[] value, int offset, int length, int result) {
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

    private static int charArrayHash(char[] value, int offset, int length, int result) {
        for (int i = offset; i < offset + length; i++) {
            result = 31 * result + value[i];
        }
        return result;
    }

    private static int shortArrayHash(short[] value, int offset, int length, int result) {
        for (int i = offset; i < offset + length; i++) {
            result = 31 * result + value[i];
        }
        return result;
    }

    private static int intArrayHash(int[] value, int offset, int length, int result) {
        for (int i = offset; i < offset + length; i++) {
            result = 31 * result + value[i];
        }
        return result;
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

    private static final class TestCase {
        private final String name;
        private final int offset;
        private final int length;
        private final int initialValue;
        private final int expected;
        private final IntSupplier actual;

        private TestCase(String name, int offset, int length, int initialValue,
                         int expected, IntSupplier actual) {
            this.name = name;
            this.offset = offset;
            this.length = length;
            this.initialValue = initialValue;
            this.expected = expected;
            this.actual = actual;
        }

        private void check() {
            int actualValue = actual.getAsInt();
            if (actualValue != expected) {
                throw new RuntimeException(name + " mismatch: offset=" + offset
                        + ", length=" + length
                        + ", initialValue=" + initialValue
                        + ", expected=" + expected
                        + ", actual=" + actualValue);
            }
        }
    }

    private interface IntSupplier {
        int getAsInt();
    }
}
