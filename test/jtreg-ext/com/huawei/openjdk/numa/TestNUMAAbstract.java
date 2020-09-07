/*
* Copyright (c) Huawei Technologies Co., Ltd. 2012-2019. All rights
reserved.
*/
package com.huawei.openjdk.numa;

import java.util.ArrayList;
import java.util.List;
import java.util.Random;
import java.util.concurrent.CountDownLatch;

import jdk.test.lib.Asserts.*;

import jdk.test.lib.Platform;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;
import sun.hotspot.WhiteBox;
/**
 * @summary Utility class.
 * @author wangruishun
 */
public  class TestNUMAAbstract {


    private static final int ARGS_LEN_LIMIT = 11;

    void threadRun(int minObjCount,int maxObjCount,int minStore,int maxStore,CountDownLatch mDoneSignal,int minThreadSleep, int maxThreadSleep){

    }
    /**
     * get random from closed interval
     *
     * @param minNum min
     * @param maxNum max
     * @return random
     */
    public static int randomNum(int minNum, int maxNum) {
        if (minNum == maxNum) {
            return minNum;
        }
        Random random = new Random();
        int randomNum = random.nextInt((maxNum - minNum) + 1) + minNum;
        return randomNum;
    }

    /**
     * start all thread
     * @param createNUMABindThread thread list
     */
    public static void runNUMABindThread(List<Thread> createNUMABindThread) {
        for (Thread thread : createNUMABindThread) {
            try {
                thread.start();
            } catch (Exception e) {
                e.printStackTrace();
            }
        }
    }

    /**
     * create thread and The execution content is provided by the caller
     *
     * @param maxThreadNum maxThreadNum
     * @param minStore minStore
     * @param maxStore maxStore
     * @param minThreadSleep minThreadSleep
     * @param maxThreadSleep maxThreadSleep
     * @param minObjCount minObjCount
     * @param maxObjCount maxObjCount
     * @return list
     */
    public static List<Thread> createNUMABindThread(int maxThreadNum, int minStore, int maxStore, int minThreadSleep, int maxThreadSleep, int minObjCount, int maxObjCount, CountDownLatch mDoneSignal,TestNUMAAbstract testNUMAAbstract) {
        System.gc();
        System.out.println("-------init gc over ------------");
        System.out.println(String.format("args[0]:Total thread count:%s", maxThreadNum));
        System.out.println(String.format("args[1]:Min thread sleep:%s(um)", minThreadSleep));
        System.out.println(String.format("args[2]:Max thread sleep:%s(um)", maxThreadSleep));
        System.out.println(String.format("args[3]:Min RAM,int array length:%s", minStore));
        System.out.println(String.format("args[4]:Max RAM,int array length:%s", maxStore));
        System.out.println(String.format("args[5]:Min count of Obj:%s", minObjCount));
        System.out.println(String.format("args[6]:Max count of Obj:%s", maxObjCount));
        List<Thread> list = new ArrayList<>();
        int i = 0;
        while (i < maxThreadNum) {
            Thread createObj = new TestNUMABindThread(minStore, maxStore, minThreadSleep, maxThreadSleep, minObjCount, maxObjCount, mDoneSignal,testNUMAAbstract);
            list.add(createObj);
            i++;
        }
        return list;
    }


    /**
     * execute class
     *
     * @param args the param of main
     * @param exeClass calss name
     * @throws Exception
     */
    public static OutputAnalyzer executeClass(String[] args,Class exeClass) throws Exception {
        final String[] arguments = {
                "-Xbootclasspath/a:.",
                "-XX:" + args[7],
                args[8],
                args[9],
                "-Xlog:gc*=info",
                exeClass.getName(),
                args[0],
                args[1],
                args[2],
                args[3],
                args[4],
                args[5],
                args[6]
        };
        ProcessBuilder pb = ProcessTools.createJavaProcessBuilder(arguments);
        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        output.shouldHaveExitValue(0);
        return output;
    }

    /**
     * param check
     * @param args
     * @return
     */
    public static boolean checkArgs(String[] args) {
        if (args == null || args.length != ARGS_LEN_LIMIT) {
            System.out.println("args[0]:Total thread count");
            System.out.println("args[1]:Min thread sleep（um）");
            System.out.println("args[2]:Max thread sleep（um）");
            System.out.println("args[3]:Min RAM,int array length");
            System.out.println("args[4]:Max RAM,int array length");
            System.out.println("args[5]:Min count of Obj");
            System.out.println("args[6]:Max count of Obj");
            System.out.println("args[7]:NUMA is open,+UseNUMA/-UseNUMA");
            return false;
        }
        return true;
    }
}


class TestNUMABindThread extends Thread {
    private int minStore;
    private int maxStore;
    private int minThreadSleep;
    private int maxThreadSleep;
    private int minObjCount;
    private int maxObjCount;
    private CountDownLatch mDoneSignal;
    private TestNUMAAbstract testNUMAAbstract;

    /**
     * @param minStore       min store
     * @param maxStore       max store
     * @param minThreadSleep sleep time(um)
     * @param maxThreadSleep sleep time(um)
     * @param minObjCount    the count of obj in one thread
     * @param maxObjCount    the count of obj in one thread
     */
    public TestNUMABindThread(int minStore, int maxStore, int minThreadSleep, int maxThreadSleep, int minObjCount, int maxObjCount, CountDownLatch mDoneSignal, TestNUMAAbstract testNUMAAbstract) {
        this.minStore = minStore;
        this.maxStore = maxStore;
        this.minThreadSleep = minThreadSleep;
        this.maxThreadSleep = maxThreadSleep;
        this.minObjCount = minObjCount;
        this.maxObjCount = maxObjCount;
        this.mDoneSignal = mDoneSignal;
        this.testNUMAAbstract = testNUMAAbstract;
    }

    @Override
    public void run() {
        testNUMAAbstract.threadRun(minObjCount, maxObjCount, minStore, maxStore, mDoneSignal,minThreadSleep,maxThreadSleep);
        mDoneSignal.countDown();
    }
}