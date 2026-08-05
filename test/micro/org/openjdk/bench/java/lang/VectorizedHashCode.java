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
package org.openjdk.bench.java.lang;

import jdk.internal.util.ArraysSupport;

import org.openjdk.jmh.annotations.Benchmark;
import org.openjdk.jmh.annotations.BenchmarkMode;
import org.openjdk.jmh.annotations.Fork;
import org.openjdk.jmh.annotations.Level;
import org.openjdk.jmh.annotations.Measurement;
import org.openjdk.jmh.annotations.Mode;
import org.openjdk.jmh.annotations.OutputTimeUnit;
import org.openjdk.jmh.annotations.Param;
import org.openjdk.jmh.annotations.Scope;
import org.openjdk.jmh.annotations.Setup;
import org.openjdk.jmh.annotations.State;
import org.openjdk.jmh.annotations.Warmup;

import java.nio.ByteOrder;
import java.util.concurrent.TimeUnit;

/**
 * Measures the five {@code ArraysSupport.vectorizedHashCode} input shapes used
 * by String and primitive array hash code callers.
 *
 * Run separate forks with VM options to compare paths, for example:
 * {@code -XX:-UseSVEHashCodeIntrinsic} for the base path and
 * {@code -XX:+UseSVEHashCodeIntrinsic -XX:UseSVE=2} for the SVE2 path.
 */
@BenchmarkMode(Mode.AverageTime)
@OutputTimeUnit(TimeUnit.NANOSECONDS)
@Warmup(iterations = 5, time = 1)
@Measurement(iterations = 5, time = 1)
@Fork(value = 1, jvmArgsAppend = {
        "-Xbatch",
        "--add-exports=java.base/jdk.internal.util=ALL-UNNAMED"
})
@State(Scope.Thread)
public class VectorizedHashCode {

    private static final int T_BOOLEAN = 4;
    private static final int T_CHAR = 5;
    private static final int T_BYTE = 8;
    private static final int T_SHORT = 9;
    private static final int T_INT = 10;

    @Param({"32", "63", "64", "65", "128", "256", "1024", "4096", "16384"})
    private int size;

    private byte[] latin1;
    private byte[] utf16;
    private byte[] bytes;
    private short[] shorts;
    private int[] ints;

    @Setup(Level.Trial)
    public void setup() {
        latin1 = new byte[size];
        utf16 = new byte[size * Character.BYTES];
        bytes = new byte[size];
        shorts = new short[size];
        ints = new int[size];

        for (int i = 0; i < size; i++) {
            latin1[i] = (byte) ((i * 131 + 0x5a) & 0xff);
            putUtf16Char(utf16, i, (char) ((i * 9973 + 0x0100) & 0xffff));
            bytes[i] = (byte) ((i * 131 + 0x5a) ^ (i >>> 1));
            shorts[i] = (short) ((i * 9973 - 0x4000) ^ (i << 3));
            ints[i] = (i * 0x9e3779b9) ^ (i << 13) ^ (i >>> 7);
        }
    }

    @Benchmark
    public int latin1() {
        return ArraysSupport.vectorizedHashCode(latin1, 0, size, 1, T_BOOLEAN);
    }

    @Benchmark
    public int utf16() {
        return ArraysSupport.vectorizedHashCode(utf16, 0, size, 1, T_CHAR);
    }

    @Benchmark
    public int byteArray() {
        return ArraysSupport.vectorizedHashCode(bytes, 0, size, 1, T_BYTE);
    }

    @Benchmark
    public int shortArray() {
        return ArraysSupport.vectorizedHashCode(shorts, 0, size, 1, T_SHORT);
    }

    @Benchmark
    public int intArray() {
        return ArraysSupport.vectorizedHashCode(ints, 0, size, 1, T_INT);
    }

    private static void putUtf16Char(byte[] value, int index, char c) {
        int byteIndex = index * Character.BYTES;
        if (ByteOrder.nativeOrder() == ByteOrder.BIG_ENDIAN) {
            value[byteIndex] = (byte) (c >>> Byte.SIZE);
            value[byteIndex + 1] = (byte) c;
        } else {
            value[byteIndex] = (byte) c;
            value[byteIndex + 1] = (byte) (c >>> Byte.SIZE);
        }
    }
}
