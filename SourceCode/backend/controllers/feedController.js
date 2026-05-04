import { getWeeklyFeedStats, triggerManualFeed } from "../services/feedService.js";
import { FeedLog } from "../models/FeedLog.js";

export const manualFeed = async (req, res) => {
  // Default amount is 5 grams
  const amount = 5;

  try {
    const feedLog = await triggerManualFeed({
      userId: req.user._id,
      amount,
    });

    return res.status(200).json({
      message: "Feeding manual command sent",
      feedLog: feedLog.toObject()
    });
  } catch (error) {
    console.error("Manual feed error:", error.message);
    return res.status(502).json({
      message: "Failed to send feeding command",
      error: error.message,
    });
  }
};

export const weeklyStats = async (req, res) => {
  const days = 7;
  
  try {
    const stats = await getWeeklyFeedStats({
      userId: req.user._id,
      days,
    });

    return res.json({
      data: stats,
      days,
    });
  } catch (error) {
    console.error("Weekly stats error:", error.message);
    return res.status(500).json({
      message: "Failed to fetch feeding stats",
      error: error.message,
    });
  }
};

export const recentFeedHistory = async (req, res) => {
  const limit = Math.min(Number(req.query.limit) || 20, 100);

  try {
    const feedLogs = await FeedLog.find({ user: req.user._id })
      .sort({ createdAt: -1 })
      .limit(limit)
      .lean();

    return res.json({
      feedLogs,
    });
  } catch (error) {
    console.error("Recent feed history error:", error.message);
    return res.status(500).json({
      message: "Failed to fetch feed history",
      error: error.message,
    });
  }
};

