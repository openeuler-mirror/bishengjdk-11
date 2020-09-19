/*
*Copyright (c) Huawei Technologies Co., Ltd. 2012-2019. All rights reserved.
*/
package com.huawei.openjdk.numa;
/**
 * @test TestNUMAARMIO
 * @key gc
 * @modules java.base/jdk.internal.misc
 * @library /test/lib
 * @build sun.hotspot.WhiteBox
 * @build com.huawei.openjdk.numa.TestNUMAAbstract
 * @run driver ClassFileInstaller sun.hotspot.WhiteBox
 * @run main/othervm -Xlog:gc*=info -Xms8G -Xmx8G -XX:+UseNUMA com.huawei.openjdk.numa.TestNUMAARMIO 100 80000 80000 0 7 10000 10000 +UseNUMA -Xms16G -Xmx16G 70
 * @summary open NUMA-Aware，test mermoy allocate and copy
 * @author wangruishun
 */

/**
 * @test TestNUMAARMIO
 * @key gc
 * @modules java.base/jdk.internal.misc
 * @library /test/lib
 * @build sun.hotspot.WhiteBox
 * @build com.huawei.openjdk.numa.TestNUMAAbstract
 * @run driver ClassFileInstaller sun.hotspot.WhiteBox
 * @run main/othervm -Xlog:gc*=info -Xms8G -Xmx8G -XX:-UseNUMA com.huawei.openjdk.numa.TestNUMAARMIO 100 80000 80000 0 7 10000 10000 -UseNUMA -Xms16G -Xmx16G 70
 * @summary close NUMA-Aware，test mermoy allocate and copy
 * @author wangruishun
 */
import jdk.test.lib.Asserts.*;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

import java.util.ArrayList;
import java.util.List;
import java.util.Random;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.regex.Matcher;
import java.util.regex.Pattern;



public class TestNUMAARMIO {


    public static void main(String[] args) throws Exception {
        if (!TestNUMAAbstract.checkArgs(args)) {
            System.err.println("[param error] please check your param");
            throw new RuntimeException("args error!");
        }
        String flagStr = args[10];
        float flag = Float.parseFloat(flagStr);
        OutputAnalyzer output = TestNUMAAbstract.executeClass(args,ExeTest.class);
        System.out.println(output.getStdout());
    }



    private static class ExeTest {

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
            //create thread
            List<Thread> threadList = TestNUMAAbstract.createNUMABindThread(threadNum, minStore, maxStore, minThreadSleep, maxThreadSleep, minObjCount, maxObjCount,mDoneSignal,new TestNUMAAbstract(){
                @Override
                void threadRun(int minObjCount, int maxObjCount, int minStore, int maxStore, CountDownLatch mDoneSignal, int minThreadSleep, int maxThreadSleep) {
                    int randomObjNum = TestNUMAAbstract.randomNum(minObjCount, maxObjCount);
                    int count = 0;
                    while (count < randomObjNum) {
                        int randomStore = TestNUMAAbstract.randomNum(minStore, maxStore);
                        int[] arr = new int[randomStore];
                        //allocate mermory
                        for (int i = 0; i < arr.length; i++) {
                            arr[i] = i;
                        }
                        //copy mermory
                        int[] tem = new int[randomStore];
                        for (int i = 0; i < arr.length; i++) {
                            tem[i] = arr[i];
                        }
                        count++;
                    }
                    mDoneSignal.countDown();
                }
            });

            TestNUMAAbstract.runNUMABindThread(threadList);
            mDoneSignal.await();
            long endTime = System.currentTimeMillis();
            System.out.println("***********end time*************" + endTime);
            System.out.println(String.format("Total thread count:%s", threadNum));
            System.out.println(String.format("Min thread sleep:%s(um)", minThreadSleep));
            System.out.println(String.format("Max thread sleep:%s(um)", maxThreadSleep));
            System.out.println(String.format("Min RAM,int array length:%s", minStore));
            System.out.println(String.format("Max RAM,int array length:%s", maxStore));
            System.out.println(String.format("Min count of Obj:%s", minObjCount));
            System.out.println(String.format("Max count of Obj:%s", maxObjCount));


            double objTotalCount = threadNum*minObjCount;
            double totalArm = objTotalCount*minStore*4;
            //byte to KB
            totalArm = totalArm/1024;
            //KB to MB
            totalArm = totalArm/1024;
            System.out.println(String.format("allocate total ARM:%sMB", totalArm));
            System.out.println(String.format("copy total ARM:%sMB", totalArm));
            System.out.println("exe time:" + (endTime - starTime));
        }





    }
}

