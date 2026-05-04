import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState } from 'react';
import { FeedAPI } from '../services/api.js';

const NotificationContext = createContext(null);
const STORAGE_KEY = 'spf_notifications';
const MAX_NOTIFICATIONS = 50;
const SCHEDULED_SEEN_KEY = 'spf_seen_scheduled_feed_logs';

const safeParse = (value) => {
  try {
    return JSON.parse(value);
  } catch (error) {
    return [];
  }
};

const loadSeenScheduledIds = () => {
  if (typeof window === 'undefined') return [];
  return safeParse(localStorage.getItem(SCHEDULED_SEEN_KEY)) || [];
};

export const NotificationProvider = ({ children }) => {
  const [notifications, setNotifications] = useState(() => {
    if (typeof window === 'undefined') return [];
    return safeParse(localStorage.getItem(STORAGE_KEY)) || [];
  });
  const [browserLocale, setBrowserLocale] = useState('vi-VN');
  const seenScheduledIdsRef = useRef(new Set(loadSeenScheduledIds()));

  useEffect(() => {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(notifications));
  }, [notifications]);

  useEffect(() => {
    if (typeof navigator !== 'undefined') {
      setBrowserLocale(navigator.language?.toLowerCase().startsWith('vi') ? 'vi-VN' : 'en-US');
    }
  }, []);

  const addNotification = useCallback((notification) => {
    setNotifications((prev) => {
      // Deduplication: Check if similar notification already exists
      // For scheduled feeds, check if same method + amount + within 2 minutes
      if (notification.method === 'scheduled') {
        const now = new Date(notification.createdAt || new Date().toISOString()).getTime();
        const duplicate = prev.find((n) => {
          if (n.method !== 'scheduled') return false;
          if (n.amount !== notification.amount) return false;
          const nTime = new Date(n.createdAt).getTime();
          // Same scheduled feed if within 2 minutes
          return Math.abs(now - nTime) < 120000; // 2 minutes in ms
        });
        
        if (duplicate) {
          // Update existing notification instead of creating duplicate
          return prev.map((n) =>
            n.id === duplicate.id
              ? {
                  ...n,
                  message: notification.message || n.message,
                  meta: { ...n.meta, ...notification.meta },
                }
              : n
          );
        }
      }

      const next = [
        {
          id: notification.id || `${Date.now()}-${Math.random().toString(16).slice(2)}`,
          message: notification.message || 'New notification',
          type: notification.type || 'info',
          amount: notification.amount ?? null,
          method: notification.method || 'manual',
          transcript: notification.transcript || null,
          createdAt: notification.createdAt || new Date().toISOString(),
          read: false,
          meta: notification.meta || {},
        },
        ...prev,
      ];
      return next.slice(0, MAX_NOTIFICATIONS);
    });
  }, []);

  const markAllRead = useCallback(() => {
    setNotifications((prev) => prev.map((item) => ({ ...item, read: true })));
  }, []);

  const clearNotifications = useCallback(() => {
    setNotifications([]);
  }, []);

  const unreadCount = useMemo(() => notifications.filter((item) => !item.read).length, [notifications]);

  useEffect(() => {
    const persistSeenScheduled = () => {
      if (typeof window === 'undefined') return;
      localStorage.setItem(SCHEDULED_SEEN_KEY, JSON.stringify([...seenScheduledIdsRef.current]));
    };

    let cancelled = false;

    const syncScheduledFeeds = async () => {
      try {
        const { data } = await FeedAPI.history(30);
        if (cancelled) return;
        const feedLogs = data.feedLogs || [];

        const scheduledLogs = feedLogs.filter(
          (log) => log.feedType === 'scheduled' && (!log.status || log.status === 'success'),
        );
        if (!scheduledLogs.length) return;

        setNotifications((prev) => {
          let added = false;
          const next = [...prev];

          scheduledLogs.forEach((log) => {
            if (seenScheduledIdsRef.current.has(log._id)) return;

            seenScheduledIdsRef.current.add(log._id);
            added = true;

            const amount = log.amount ?? log.targetAmount ?? 5;
            const message =
              browserLocale === 'vi-VN'
                ? `Lịch cho ăn ${amount}g thành công`
                : `Scheduled feed ${amount}g completed`;

            next.unshift({
              id: `feedlog-${log._id}`,
              message,
              type: 'info',
              amount,
              method: 'scheduled',
              createdAt: log.createdAt || log.startTime || new Date().toISOString(),
              read: false,
              meta: {
                feedLogId: log._id,
              },
            });
          });

          if (added) {
            persistSeenScheduled();
            return next.slice(0, MAX_NOTIFICATIONS);
          }
          return prev;
        });
      } catch (error) {
        // silent fail, avoid spamming logs
      }
    };

    syncScheduledFeeds();
    const interval = setInterval(syncScheduledFeeds, 30000);

    return () => {
      cancelled = true;
      clearInterval(interval);
    };
  }, [browserLocale]);

  const value = useMemo(
    () => ({
      notifications,
      addNotification,
      markAllRead,
      clearNotifications,
      unreadCount,
    }),
    [notifications, addNotification, markAllRead, clearNotifications, unreadCount],
  );

  return <NotificationContext.Provider value={value}>{children}</NotificationContext.Provider>;
};

export const useNotifications = () => {
  const context = useContext(NotificationContext);
  if (!context) {
    throw new Error('useNotifications must be used within a NotificationProvider');
  }
  return context;
};


