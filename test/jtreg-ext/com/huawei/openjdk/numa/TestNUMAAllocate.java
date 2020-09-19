/*
* Copyright (c) Huawei Technologies Co., Ltd. 2012-2019. All rights
reserved.
*/
package com.huawei.openjdk.numa;
/**
 * @test TestNUMAAllocate
 * @key gc
 * @modules java.base/jdk.internal.misc
 * @library /test/lib
 * @build sun.hotspot.WhiteBox
 * @build com.huawei.openjdk.numa.TestNUMAAbstract
 * @run driver ClassFileInstaller sun.hotspot.WhiteBox
 * @run main/othervm -Xlog:gc*=info -Xms8G -Xmx8G -XX:+UseNUMA com.huawei.openjdk.numa.TestNUMAAllocate 1500 77000 80000 0 7 10000 10000 +UseNUMA -Xms8G -Xmx8G 70
 * @summary opem NUMA-Aware,Memory allocate distribution ratio exceeds 70%
 * @author wangruishun
 */
/**
 * @test TestNUMAAllocate
 * @key gc
 * @modules java.base/jdk.internal.misc
 * @library /test/lib
 * @build sun.hotspot.WhiteBox
 * @build com.huawei.openjdk.numa.TestNUMAAbstract
 * @run driver ClassFileInstaller sun.hotspot.WhiteBox
 * @run main/othervm -Xlog:gc*=info -Xms16G -Xmx16G -XX:+UseNUMA com.huawei.openjdk.numa.TestNUMAAllocate 1 6000000 9000000 0 0 100 100 +UseNUMA -Xms8G -Xmx16G 20
 * @summary opem NUMA-Aware,Memory allocate distribution ratio exceeds 20%,one thread Humongous.
 * @author wangruishun
 */
/**
 * @test TestNUMAAllocate
 * @key gc
 * @modules java.base/jdk.internal.misc
 * @library /test/lib
 * @build sun.hotspot.WhiteBox
 * @build com.huawei.openjdk.numa.TestNUMAAbstract
 * @run driver ClassFileInstaller sun.hotspot.WhiteBox
 * @run main/othervm -Xlog:gc*=info -Xms16G -Xmx16G -XX:+UseNUMA com.huawei.openjdk.numa.TestNUMAAllocate 5 800000 1000000 0 7 100 100 +UseNUMA -Xms256M -Xmx16G 45
 * @summary opem NUMA-Aware,Memory allocate distribution ratio exceeds 45%,5 thread，Humongous
 * @author wangruishun
 */

/**
 * @test TestNUMAAllocate
 * @key gc
 * @modules java.base/jdk.internal.misc
 * @library /test/lib
 * @build sun.hotspot.WhiteBox
 * @build com.huawei.openjdk.numa.TestNUMAAbstract
 * @run driver ClassFileInstaller sun.hotspot.WhiteBox
 * @run main/othervm -Xlog:gc*=info -Xms16G -Xmx16G -XX:+UseNUMA com.huawei.openjdk.numa.TestNUMAAllocate 5 800000 1000000 0 7 100 100 +UseNUMA -Xms256M -Xmx16G 45
 * @summary opem NUMA-Aware,Memory allocate distribution ratio exceeds 45%,5 thread，Humongous
 * @author wangruishun
 */


/**
 * @test TestNUMAAllocate
 * @key gc
 * @modules java.base/jdk.internal.misc
 * @library /test/lib
 * @build sun.hotspot.WhiteBox
 * @build com.huawei.openjdk.numa.TestNUMAAbstract
 * @run driver ClassFileInstaller sun.hotspot.WhiteBox
 * @run main/othervm -Xlog:gc*=info -Xms8G -Xmx8G -XX:+UseNUMA com.huawei.openjdk.numa.TestNUMAAllocate 120 77000 80000 0 7 150 150 +UseNUMA -Xms8G -Xmx8G 70
 * @summary opem NUMA-Aware,Memory allocate distribution ratio exceeds 70%
 * @author wangruishun
 */


import java.util.ArrayList;
import java.util.List;
import java.util.Random;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import jdk.test.lib.Asserts.*;

import jdk.test.lib.Platform;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;
import sun.hotspot.WhiteBox;


public class TestNUMAAllocate{

    private static final int ARGS_LEN_LIMIT = 11;

    public static void main(String[] args) throws Exception {
        if (!TestNUMAAbstract.checkArgs(args)) {
            System.err.println("[param error] please check your param");
            throw new RuntimeException("args error!");
        }
        //ratio
        String flagStr = args[10];
        float flag = Float.parseFloat(flagStr);
        //execute program and get stdout
        OutputAnalyzer output = TestNUMAAbstract.executeClass(args,GClogTest.class);
        //check print
        checkPattern(".*Placement match ratio:*", output.getStdout(),flag);
    }



    /**
     * Check if the string matches
     *
     * @param pattern string
     * @param what string
     * @param flag ratio
     * @throws Exception
     */
    private static void checkPattern(String pattern, String what, float flag) throws Exception {
        String[] arr = what.split(System.lineSeparator());
        boolean isMatch = false;
        float maxPercent = 0f;
        for (String line : arr) {
            Pattern r = Pattern.compile(pattern);
            Matcher m = r.matcher(line);
            if (m.find()) {
                isMatch = true;
                Float percentLine = getPercentByLog(line);
                if (percentLine > maxPercent) {
                    maxPercent = percentLine;
                }
            }
        }
        System.out.println(String.format("NUMA percent:%s", maxPercent));
        if (!isMatch) {
            throw new RuntimeException("Could not find pattern " + pattern + " in output");
        }
        if (maxPercent < flag) {
            throw new RuntimeException("MUMA Seems to fail to start ");
        }
    }

    /**
     * get ration by gclog
     *
     * @param line
     * @return
     */
    private static Float getPercentByLog(String line) {
        if (null == line || "".equals(line)) {
            return 0f;
        }
        //[1.631s][info][gc,heap,numa   ] GC(23) Placement match ratio: 5% 555/10618 (0: 62% 243/392, 1: 53% 265/498, 2: 100% 11/11, 3: 100% 36/36)
        Pattern pattern = Pattern.compile(".\\d%|[1-9]*%|100%");
        Matcher matcher = pattern.matcher(line);
        Float percent = 0f;
        if(matcher.find()){
            String percentStr = matcher.group(0);
            percentStr = percentStr.substring(0,percentStr.length()-1);
            percent = Float.parseFloat(percentStr);
        }
        return percent;
    }


    private static class GClogTest {
        public static void main(String[] args) throws Exception {
            int threadNum = Integer.valueOf(args[0]).intValue();
            int minStore = Integer.valueOf(args[1]).intValue();
            int maxStore = Integer.valueOf(args[2]).intValue();
            int minThreadSleep = Integer.valueOf(args[3]).intValue();
            int maxThreadSleep = Integer.valueOf(args[4]).intValue();
            int minObjCount = Integer.valueOf(args[5]).intValue();
            int maxObjCount = Integer.valueOf(args[6]).intValue();
            long starTime = System.currentTimeMillis();
            System.out.println("***********star time*************:" + starTime);
            final CountDownLatch mDoneSignal = new CountDownLatch(threadNum);
            List<Thread> threadList = TestNUMAAbstract.createNUMABindThread(threadNum, minStore, maxStore, minThreadSleep, maxThreadSleep, minObjCount, maxObjCount,mDoneSignal,new TestNUMAAbstract(){
                @Override
                void threadRun(int minObjCount, int maxObjCount, int minStore, int maxStore, CountDownLatch mDoneSignal, int minThreadSleep, int maxThreadSleep) {
                    int randomObjNum = TestNUMAAbstract.randomNum(minObjCount, maxObjCount);
                    int count = 0;
                    while (count < randomObjNum) {
                        int randomStore = TestNUMAAbstract.randomNum(minStore, maxStore);
                        int[] arr = new int[randomStore];
                        int[] tem = new int[1];
                        for (int i = 0; i < arr.length; i++) {
                            tem[0] = arr[i];
                        }

                        count++;
                        try {
                            int threadSleep = TestNUMAAbstract.randomNum(minThreadSleep, maxThreadSleep);
                            TimeUnit.MICROSECONDS.sleep(threadSleep);
                        } catch (InterruptedException e) {
                            e.printStackTrace();
                        }
                    }
                    mDoneSignal.countDown();
                }
            });
            TestNUMAAbstract.runNUMABindThread(threadList);
            mDoneSignal.await();
            long endTime = System.currentTimeMillis();
            System.out.println("***********end time*************" + endTime);
            System.out.println("***********result time*************" + (starTime - endTime));
        }

    }
}


