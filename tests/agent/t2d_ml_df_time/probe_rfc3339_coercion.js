import { formatRFC3339 } from "dyna:time";
function t(label, fn) {
    try {
        const r = fn();
        console.log(label, "->", JSON.stringify(r));
    } catch (e) {
        console.log(label, "-> THROWS", e.constructor.name, JSON.stringify(String(e.message)));
    }
}
t('legacy(0, "5")', () => formatRFC3339(0, "5"));
t('legacy(0, true)', () => formatRFC3339(0, true));
t('legacy(0, false)', () => formatRFC3339(0, false));
t('legacy(0, null)', () => formatRFC3339(0, null));
t('legacy(0, [])', () => formatRFC3339(0, []));
t('legacy(0, [7])', () => formatRFC3339(0, [7]));
t('legacy(0, {})', () => formatRFC3339(0, {}));
t('legacy(0, 5n)', () => formatRFC3339(0, 5n));
t('legacy(0, 123456789)', () => formatRFC3339(0, 123456789));
t('legacy(0, 1.5)', () => formatRFC3339(0, 1.5));
t('legacy(0, -1)', () => formatRFC3339(0, -1));
t('legacy(0, 1e9)', () => formatRFC3339(0, 1e9));
t('legacy(0, undefined, "yes")', () => formatRFC3339(0, undefined, "yes"));
t('legacy(0, undefined, 3)', () => formatRFC3339(0, undefined, 3));
t('legacy(0, 5, "yes")', () => formatRFC3339(0, 5, "yes"));
t('legacy(0, Symbol())', () => formatRFC3339(0, Symbol()));
t('legacy(0, 0/0)', () => formatRFC3339(0, 0/0));
t('bag nsec "5"', () => formatRFC3339(0, { nsec: "5" }));
t('bag nsec true', () => formatRFC3339(0, { nsec: true }));
t('bag nsec 5n', () => formatRFC3339(0, { nsec: 5n }));
t('bag nsec 1.5', () => formatRFC3339(0, { nsec: 1.5 }));
t('bag off "30"', () => formatRFC3339(0, { offsetMinutes: "30" }));
t('bag off true', () => formatRFC3339(0, { offsetMinutes: true }));
t('bag off 30n', () => formatRFC3339(0, { offsetMinutes: 30n }));
t('bag off 1.5', () => formatRFC3339(0, { offsetMinutes: 1.5 }));
t('legacy(0, {valueOf(){return 7}})', () => formatRFC3339(0, { valueOf() { return 7; } }));
