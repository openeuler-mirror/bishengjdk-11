/*
 * Copyright (c) 2021, Huawei Technologies Co., Ltd. All rights reserved.
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
 * @summary Test whether LazyBox works
 * @run main/othervm -XX:+UnlockExperimentalVMOptions -XX:+LazyBox
 *                   -XX:+AggressiveUnboxing
 *                   -XX:CompileCommand=dontinline,compiler/lazybox/TestLazyBox.foo
 *                   -XX:CompileCommand=dontinline,compiler/lazybox/TestLazyBox.black
 *                   -Xcomp -XX:-TieredCompilation
 *                   compiler.lazybox.TestLazyBox
 */

package compiler.lazybox;

class Wrap {
    public Object field;
}

public class TestLazyBox {
    public static Integer base = null;

    public static void main(String[] args) throws Exception{
      for(int i = 0; i < 10; i++) {
        Integer IntObj2 = Integer.valueOf(i);
        foo(IntObj2);
      }
      System.out.println("Passed");
    }

    public static void foo(Integer IntObj2) throws Exception {

      Integer IntObj = Integer.valueOf(299);
      base = IntObj;
      try{
        black(IntObj);

        if (IntObj == IntObj2) {
            black(IntObj);
        }

        if (IntObj.equals(IntObj2)) {
            black(IntObj);
        }

        Wrap wrap = new Wrap();
        wrap.field = IntObj;

        black(wrap.field);
      } catch(Exception e) {
        e.printStackTrace();
      }
    }

    public static void black(Object i) {
      if (base == i) {
        System.err.println("Failed tests.");
        throw new InternalError();
      }
    }
}
