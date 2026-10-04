/**
 * Spelling-tolerant search key for Tamil place names written in English:
 * Madhuravoyal = Maduravoyal, Sholinganallur = Shozhinganallur,
 * Thiruvottiyur = Tiruvottiyur.
 */
export function normName(s: string): string {
  return s
    .toLowerCase()
    .replace(/\((sc|st)\)/g, "")
    .replace(/[^a-z]/g, "")
    .replace(/zh/g, "l")
    .replace(/([bdgkpst])h/g, "$1")
    .replace(/ee/g, "i")
    .replace(/oo/g, "u")
    .replace(/w/g, "v")
    .replace(/(.)\1+/g, "$1");
}
