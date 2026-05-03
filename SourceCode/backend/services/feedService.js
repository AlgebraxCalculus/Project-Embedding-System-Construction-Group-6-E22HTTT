import { FeedLog } from "../models/FeedLog.js";
import { publishFeedCommand } from "./mqttService.js";

const DEFAULT_TIMEZONE = process.env.APP_TIMEZONE || "Asia/Ho_Chi_Minh";

export const triggerManualFeed = async ({ userId, amount }) => {
  const feedLog = new FeedLog({
    user: userId,
    feedType: "manual",
    amount: 0, // will be updated from ACK
    targetAmount: amount,
    startTime: new Date(),
  });

  // prepare payload and ensure issuedAt present for correlation
  const payload = {
    mode: "manual",
    amount,
    userId,
    issuedAt: Date.now(),
  };

  try {
    // publish and wait for device ACK (will resolve with parsed ACK JSON from device)
    const ack = await publishFeedCommand(payload);

    // ack expected shape: { device_id, timestamp, type: "feeding_complete", mode, amount, targetAmount, status, userId, issuedAt }
    feedLog.amount = ack.amount ?? 0;
    feedLog.targetAmount = ack.targetAmount ?? amount;
    feedLog.status = ["success", "hopper_empty"].includes(ack.status) ? ack.status : "failed";
    feedLog.endTime = new Date();

    await feedLog.save();
    return feedLog;
  } catch (error) {
    // publish failed or ACK timeout
    feedLog.status = "failed";
    feedLog.endTime = new Date();
    await feedLog.save();
    throw error;
  }
};

export const triggerScheduledFeed = async ({ userId, amount, scheduleId }) => {
  const feedLog = new FeedLog({
    user: userId,
    feedType: "scheduled",
    amount: 0,
    targetAmount: amount,
    schedule: scheduleId,
    startTime: new Date(),
  });

  const payload = {
    mode: "scheduled",
    amount,
    userId,
    scheduleId,
    issuedAt: Date.now(),
  };

  try {
    const ack = await publishFeedCommand(payload);

    feedLog.amount = ack.amount ?? 0;
    feedLog.targetAmount = ack.targetAmount ?? amount;
    feedLog.status = ["success", "hopper_empty"].includes(ack.status) ? ack.status : "failed";
    feedLog.endTime = new Date();

    await feedLog.save();
    return feedLog;
  } catch (error) {
    feedLog.status = "failed";
    feedLog.endTime = new Date();
    await feedLog.save();
    throw error;
  }
};

export const triggerVoiceFeed = async ({ userId, amount, voiceCommand }) => {
  const feedLog = new FeedLog({
    user: userId,
    feedType: "voice",
    amount: 0,
    targetAmount: amount,
    voiceCommand,
    startTime: new Date(),
  });

  const payload = {
    mode: "voice",
    amount,
    userId,
    voiceCommand,
    issuedAt: Date.now(),
  };

  try {
    const ack = await publishFeedCommand(payload);

    feedLog.amount = ack.amount ?? 0;
    feedLog.targetAmount = ack.targetAmount ?? amount;
    feedLog.status = ["success", "hopper_empty"].includes(ack.status) ? ack.status : "failed";
    feedLog.endTime = new Date();

    await feedLog.save();
    return feedLog;
  } catch (error) {
    feedLog.status = "failed";
    feedLog.endTime = new Date();
    await feedLog.save();
    throw error;
  }
};

export const getWeeklyFeedStats = async ({
  userId,
  days = 7,
  timezone = DEFAULT_TIMEZONE,
}) => {
  const now = new Date();
  const startDate = new Date(now);
  startDate.setDate(startDate.getDate() - (days - 1));
  startDate.setHours(0, 0, 0, 0);

  const rawStats = await FeedLog.aggregate([
    {
      $match: {
        user: userId,
        startTime: { $gte: startDate },
      },
    },
    {
      $project: {
        amount: 1,
        feedType: 1,
        date: {
          $dateToString: {
            format: "%Y-%m-%d",
            date: "$startTime",
            timezone,
          },
        },
      },
    },
    {
      $group: {
        _id: "$date",
        totalAmount: { $sum: "$amount" },
        feedCount: { $sum: 1 },
      },
    },
    { $sort: { _id: 1 } },
  ]);

  const statsMap = new Map();
  rawStats.forEach((item) => {
    statsMap.set(item._id, {
      date: item._id,
      totalAmount: item.totalAmount,
      feedCount: item.feedCount,
    });
  });

  const response = [];
  for (let i = days - 1; i >= 0; i -= 1) {
    const date = new Date(now);
    date.setDate(now.getDate() - i);
    const dateKey = date.toISOString().split("T")[0];
    response.push(
      statsMap.get(dateKey) || {
        date: dateKey,
        totalAmount: 0,
        feedCount: 0,
      }
    );
  }

  return response;
};

