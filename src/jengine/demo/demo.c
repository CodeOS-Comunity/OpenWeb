/*
 * Jengine demo - exercises the JavaScript engine end to end.
 *
 * Each snippet is evaluated with jengine_eval(); the completion value is
 * printed, or the exception if evaluation failed.
 */

#include "jengine/jengine.h"
#include <stdio.h>

static void run(jengine_context* ctx, const char* label, const char* src) {
    jengine_value result;
    int rc = jengine_eval(ctx, src, "demo", &result);
    printf("%-34s => ", label);
    if (rc == 0) {
        jengine_print_value(ctx, result);
    } else {
        printf("error: ");
        jengine_print_value(ctx, jengine_get_exception(ctx));
    }
    printf("\n");
}

int main(void) {
    jengine_context* ctx = jengine_create();
    if (!ctx) {
        fprintf(stderr, "Failed to create Jengine context\n");
        return 1;
    }

    printf("=== Values and literals ===\n");
    run(ctx, "{}", "{}");
    run(ctx, "{a: 1, b: 2}", "{a: 1, b: 2}");
    run(ctx, "{x: 42}.x", "{x: 42}.x");
    run(ctx, "[10, 20, 30][1]", "[10, 20, 30][1]");
    run(ctx, "'text' + 1", "'text' + 1");

    printf("\n=== Arithmetic & operators ===\n");
    run(ctx, "1 + 2", "1 + 2");
    run(ctx, "1 + 2 * 3", "1 + 2 * 3");
    run(ctx, "(1 + 2) * 3", "(1 + 2) * 3");
    run(ctx, "7 / 2", "7 / 2");
    run(ctx, "2 ** 10", "2 ** 10");
    run(ctx, "10 / 0", "10 / 0");
    run(ctx, "1 == '1'", "1 == '1'");
    run(ctx, "1 === '1'", "1 === '1'");
    run(ctx, "3 > 2 ? 'yes' : 'no'", "3 > 2 ? 'yes' : 'no'");
    run(ctx, "null ?? 'fallback'", "null ?? 'default'");

    printf("\n=== Strings ===\n");
    run(ctx, "'hello'.toUpperCase()", "'hello'.toUpperCase()");
    run(ctx, "'a,b,c'.split(',').join('-')", "'a,b,c'.split(',').join('-')");
    run(ctx, "'5'.padStart(3, '0')", "'5'.padStart(3, '0')");
    run(ctx, "'hello'.split('').reverse().join('')",
        "'hello'.split('').reverse().join('')");

    printf("\n=== Arrays ===\n");
    run(ctx, "[3,1,2].sort()", "[3,1,2].sort()");
    run(ctx, "[1,2,3].map(x => x * 2)", "[1,2,3].map(x => x * 2)");
    run(ctx, "[1,2,3,4].filter(x => x % 2 === 0)", "[1,2,3,4].filter(x => x % 2 === 0)");
    run(ctx, "[1,2,3,4].reduce((a,b) => a+b, 0)", "[1,2,3,4].reduce((a,b) => a+b, 0)");
    run(ctx, "[1,[2,[3]]].flat(2)", "[1,[2,[3]]].flat(2)");

    printf("\n=== Functions & closures ===\n");
    run(ctx, "function add(a,b){return a+b} add(2,3)",
            "function add(a,b){return a+b} add(2,3)");
    run(ctx, "(n => n < 2 ? n : n + 1)(4)", "(n => n < 2 ? n : n + 1)(4)");
    run(ctx, "counter closure()",
            "function counter(){var c=0;return function(){c++;return c;}}"
            "var nex = counter(); nex(); nex();");
    run(ctx, "default & rest params",
            "function f(a, b = 10, ...r){return a+b+rest_sum(rest=rest)}"
            "function sum(){var t=0;for(var i in arguments)t+=arguments[i];return t;}"
            "(function(a,b=10){return a+b})(5)");

    printf("\n=== Control flow ===\n");
    run(ctx, "for loop sum", "var t=0; for (var i=0;i<5;i++){t+=i;} t");
    run(ctx, "while loop",
            "var n=1; while(n<50){n*=2;} n");
    run(ctx, "switch", "var r=''; switch(2){case 1:r='a';break;default:r='b'} r");

    printf("\n=== Builtins ===\n");
    run(ctx, "Math.hypot(3, 4)", "Math.hypot(3, 4)");
    run(ctx, "Math.max.apply(null, [1,9,3])", "Math.max.apply(null, [1,9,3])");
    run(ctx, "JSON.stringify({a:1,b:[2,3]})", "JSON.stringify({a:1, b:[2,3]})");
    run(ctx, "parseInt('42px')", "parseInt('42px')");
    run(ctx, "Object.assign({a:1},{b:2})", "Object.assign({a:1},{b:2})");

    printf("\n=== Error handling ===\n");
    run(ctx, "try/catch",
            "var out; try { throw new TypeError('bad'); }"
            " catch (e) { out = e.name + ': ' + e.message; } out");
    run(ctx, "reference error", "missingName + 1");
    run(ctx, "type error", "null.x");

    jengine_destroy(ctx);
    printf("\nJengine demo completed\n");
    return 0;
}
