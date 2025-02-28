/*
 * Copyright (c) 2020, 2024, Huawei Technologies Co., Ltd. All rights reserved.
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

/*
 * @test
 * @summary Test JBolt timing rescheduling functions.
 * @library /test/lib
 * @requires vm.flagless
 *
 * @run driver compiler.codecache.jbolt.JBoltReschedulingTest
 */

package compiler.codecache.jbolt;

import java.io.File;
import java.io.IOException;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;
import jdk.test.lib.Utils;

public class JBoltReschedulingTest {
    public static final int LONG_LENGTH = 1025;

    private static void testNormalUse() throws Exception {
      ProcessBuilder pb1 = ProcessTools.createJavaProcessBuilder(
        "-XX:+UnlockExperimentalVMOptions",
        "-XX:+UseJBolt",
        "-XX:JBoltRescheduling=07:30,14:30,21:30",
        "-Xlog:jbolt*=trace",
        "--version"
      );

      ProcessBuilder pb2 = ProcessTools.createJavaProcessBuilder(
        "-XX:+UnlockExperimentalVMOptions",
        "-XX:+UseJBolt",
        "-XX:JBoltRescheduling=00:30,01:30,02:30,03:30,03:30,04:30,05:30,06:30,07:30,08:30,09:30,10:30",
        "-Xlog:jbolt*=trace",
        "--version"
      );

      String stdout;

      OutputAnalyzer out1 = new OutputAnalyzer(pb1.start());
      stdout = out1.getStdout();
      if (!stdout.contains("Set time trigger at 07:30") && !stdout.contains("Set time trigger at 14:30") && !stdout.contains("Set time trigger at 21:30")) {
          throw new RuntimeException(stdout);
      }
      out1.shouldHaveExitValue(0);

      OutputAnalyzer out2 = new OutputAnalyzer(pb2.start());
      stdout = out2.getStdout();
      // 03:30 is duplicate and 10:30 above max time length(10)
      if (!stdout.contains("Set time trigger at 09:30") || stdout.contains("Set time trigger at 10:30")) {
        throw new RuntimeException(stdout);
      }
      out2.shouldHaveExitValue(0);
    }

    private static void testErrorCases() throws Exception {
      ProcessBuilder pb1 = ProcessTools.createJavaProcessBuilder(
        "-XX:+UnlockExperimentalVMOptions",
        "-XX:JBoltRescheduling=07:30,14:30,21:30",
        "-Xlog:jbolt*=trace",
        "--version"
      );

      StringBuilder sb = new StringBuilder(LONG_LENGTH);
      for (int i = 0; i < LONG_LENGTH; ++i) {
        sb.append('a');
      }
      String long_str = sb.toString();
      ProcessBuilder pb2 = ProcessTools.createJavaProcessBuilder(
        "-XX:+UnlockExperimentalVMOptions",
        "-XX:+UseJBolt",
        "-XX:JBoltRescheduling=" + long_str,
        "-Xlog:jbolt*=trace",
        "--version"
      );

      ProcessBuilder pb3 = ProcessTools.createJavaProcessBuilder(
        "-XX:+UnlockExperimentalVMOptions",
        "-XX:+UseJBolt",
        "-XX:JBoltRescheduling=12:303",
        "-Xlog:jbolt*=trace",
        "--version"
      );

      ProcessBuilder pb4 = ProcessTools.createJavaProcessBuilder(
        "-XX:+UnlockExperimentalVMOptions",
        "-XX:+UseJBolt",
        "-XX:JBoltRescheduling=1:30",
        "-Xlog:jbolt*=trace",
        "--version"
      );

      ProcessBuilder pb5 = ProcessTools.createJavaProcessBuilder(
        "-XX:+UnlockExperimentalVMOptions",
        "-XX:+UseJBolt",
        "-XX:JBoltRescheduling=12.30",
        "-Xlog:jbolt*=trace",
        "--version"
      );

      ProcessBuilder pb6 = ProcessTools.createJavaProcessBuilder(
        "-XX:+UnlockExperimentalVMOptions",
        "-XX:+UseJBolt",
        "-XX:JBoltRescheduling=24:61",
        "-Xlog:jbolt*=trace",
        "--version"
      );

      OutputAnalyzer out1 = new OutputAnalyzer(pb1.start());

      out1.stdoutShouldContain("Do not set VM option JBoltRescheduling without UseJBolt enabled.");
      out1.shouldHaveExitValue(1);

      OutputAnalyzer out2 = new OutputAnalyzer(pb2.start());

      out2.stdoutShouldContain("JBoltRescheduling is too long");
      out2.shouldHaveExitValue(1);

      OutputAnalyzer out3 = new OutputAnalyzer(pb3.start());

      out3.stdoutShouldContain("Invalid time 12:303 in JBoltRescheduling");
      out3.shouldHaveExitValue(1);

      OutputAnalyzer out4 = new OutputAnalyzer(pb4.start());

      out4.stdoutShouldContain("Invalid time 1:30 in JBoltRescheduling");
      out4.shouldHaveExitValue(1);

      OutputAnalyzer out5 = new OutputAnalyzer(pb5.start());

      out5.stdoutShouldContain("Invalid time 12.30 in JBoltRescheduling");
      out5.shouldHaveExitValue(1);

      OutputAnalyzer out6 = new OutputAnalyzer(pb6.start());

      out6.stdoutShouldContain("Invalid time 24:61 in JBoltRescheduling");
      out6.shouldHaveExitValue(1);
    }

    public static void main(String[] args) throws Exception {
      testNormalUse();
      testErrorCases();
    }
}