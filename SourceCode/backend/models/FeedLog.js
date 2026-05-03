import mongoose from "mongoose";

const feedLogSchema = new mongoose.Schema(
  {
    user: {
      type: mongoose.Schema.Types.ObjectId,
      ref: "User",
      required: true,
    },
    feedType: {
      type: String,
      enum: ["manual", "scheduled", "voice"],
      required: true,
    },
    amount: {
      type: Number,
      required: true,
      min: 0,
    },
    targetAmount: {
      type: Number,
      min: 1,
    },
    status: {
      type: String,
      enum: ["success", "failed", "hopper_empty"],
      default: "success",
    },
    startTime: {
      type: Date,
      default: Date.now,
    },
    endTime: {
      type: Date,
      default: null,
    },
    schedule: {
      type: mongoose.Schema.Types.ObjectId,
      ref: "Schedule",
      default: null,
    },
    voiceCommand: {
      type: String,
      default: null,
    },
  },
  {
    timestamps: true,
  }
);

export const FeedLog = mongoose.model("FeedLog", feedLogSchema);

