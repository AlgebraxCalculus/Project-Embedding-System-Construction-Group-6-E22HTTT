// Sao chép nguyên từ frontend/src/utils/notificationHelpers.js
const hasScheduleMarkers = (text = '') => {
  if (!text) return false;
  const normalized = text.toLowerCase();
  return normalized.includes('schedule') || normalized.includes('sched') || normalized.includes('lịch');
};

const flattenNumberSearch = (value) => {
  if (typeof value === 'number' && value > 0) return value;
  if (value && typeof value === 'object') {
    if (Array.isArray(value)) {
      for (const item of value) {
        const found = flattenNumberSearch(item);
        if (found) return found;
      }
    } else {
      for (const key of Object.keys(value)) {
        const found = flattenNumberSearch(value[key]);
        if (found) return found;
      }
    }
  }
  return null;
};

export const isScheduledAckPayload = (payload) => {
  if (!payload) return false;
  if (typeof payload === 'string') return hasScheduleMarkers(payload);
  if (typeof payload !== 'object') return false;

  const mode = (payload.mode || payload.feedType || payload.method || payload.type || '')
    .toString()
    .toLowerCase();
  if (mode.includes('sched')) return true;

  if (payload.scheduleId || payload.schedule_id || payload.schedule || payload.scheduleID || payload.scheduleName)
    return true;

  if (typeof payload.message === 'string' && hasScheduleMarkers(payload.message)) return true;

  return hasScheduleMarkers(JSON.stringify(payload));
};

export const extractAckAmount = (payload, fallback = 5) => {
  if (!payload) return fallback;
  if (typeof payload === 'object') {
    if (typeof payload.amount === 'number') return payload.amount;
    if (typeof payload.targetAmount === 'number') return payload.targetAmount;
    if (payload.meta && typeof payload.meta.amount === 'number') return payload.meta.amount;
    const flattened = flattenNumberSearch(payload.data || payload.payload);
    if (flattened) return flattened;
  }
  if (typeof payload === 'string') {
    const match = payload.match(/(\d+)\s*(gram|gr|g)/i);
    if (match) {
      const parsed = parseInt(match[1], 10);
      if (!Number.isNaN(parsed) && parsed > 0) return parsed;
    }
  }
  return fallback;
};
