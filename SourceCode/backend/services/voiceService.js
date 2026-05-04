// Vietnamese trigger phrases: "cho ăn", "cho an", hoặc "ăn"/"an" đứng một mình.
// Dạng đứng một mình yêu cầu word-boundary (đầu chuỗi/khoảng trắng phía trước,
// khoảng trắng/dấu câu/cuối chuỗi phía sau) để tránh match nhầm với "ban", "anh", v.v.
const VI_FEED_TRIGGER_REGEX = /(?:^|\s)(?:cho\s+)?(?:ăn|an)(?=\s|$|[.,!?])/i;
// English trigger phrases: "feed", "give food", "dispense"
const EN_FEED_TRIGGER_REGEX = /feed|give\s+food|dispense/i;

// Amount regex - supports both Vietnamese and English
// Matches: "200 gram", "200 grams", "200 gr", "200 g"
const AMOUNT_REGEX = /(\d+)\s*(gram|gr|g|grams)\b/i;

/**
 * Parse voice command text to extract feeding instruction
 * @param {String} text - Voice command text
 * @returns {Object} { shouldFeed: Boolean, amount: Number|null, error: String|null }
 */
export const parseVoiceCommand = (text = "") => {
  if (!text || typeof text !== "string") {
    return { shouldFeed: false, amount: null, error: "Invalid text input" };
  }

  const normalized = text.trim().toLowerCase();
  
  // Check for feed trigger phrase (Vietnamese or English)
  const hasViTrigger = VI_FEED_TRIGGER_REGEX.test(normalized);
  const hasEnTrigger = EN_FEED_TRIGGER_REGEX.test(normalized);
  const hasTrigger = hasViTrigger || hasEnTrigger;
  
  if (!hasTrigger) {
    return { 
      shouldFeed: false, 
      amount: null, 
      error: "Không tìm thấy cụm từ kích hoạt trong lệnh. Tiếng Việt: 'ăn' hoặc 'cho ăn', Tiếng Anh: 'feed' (ví dụ: 'ăn 200 gram', 'cho ăn 200 gram' hoặc 'feed 200 grams')"
    };
  }

  // Extract amount (supports both Vietnamese and English)
  // If no amount specified, default to 5 grams
  const DEFAULT_AMOUNT = 5;
  const amountMatch = normalized.match(AMOUNT_REGEX);
  
  let amount = DEFAULT_AMOUNT; // Default amount if not specified
  
  if (amountMatch) {
    amount = Number(amountMatch[1]);
    if (Number.isNaN(amount) || amount <= 0) {
      // If invalid amount, use default
      amount = DEFAULT_AMOUNT;
    } else {
      // Validate amount range
      if (amount < 5 || amount > 1000) {
        return { 
          shouldFeed: false, 
          amount: null, 
          error: "Số lượng phải từ 5 đến 1000 gram" 
        };
      }
    }
  }
  // If no amount found, use default (5g)

  return { shouldFeed: true, amount, error: null };
};

