/**
 * @file The Asuka language's grammar, for tree-sitter (editors: Zed, Neovim,
 * Helix). lang/syntax is the language's parser and GRAMMAR.md its grammar;
 * this one follows them for highlighting, outlines, indentation and text
 * objects, and is tested by parsing every .asuka file in the repository.
 * @license MIT
 */

/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

// The keywords no name may be (GRAMMAR.md's, less the soft ones).
const HARD_KEYWORDS = [
  'as', 'async', 'await', 'break', 'catch', 'continue', 'defer', 'do', 'else',
  'emit', 'extend', 'false', 'fn', 'for', 'if', 'import', 'in', 'let', 'load',
  'match', 'mut', 'none', 'pub', 'reload', 'return', 'self', 'spawn', 'throw',
  'true', 'try', 'var', 'while', 'worker',
];

// Keywords only where they start a declaration or an item (decision 10):
// names everywhere else.
const SOFT_KEYWORDS = [
  'app', 'bind', 'component', 'effect', 'enum', 'event', 'extern', 'interface',
  'key', 'native', 'on', 'prop', 'provide', 'screen', 'state', 'store', 'struct',
  'style', 'task', 'test', 'throws', 'type', 'use',
];

// Words that are keywords only in one production; names elsewhere.
const CONTEXTUAL = [
  'shader', 'at', 'static', 'variant', 'cleanup', 'with', 'release', 'private',
  'bench', 'measure', 'preview',
];

const PREC = {
  lambda: -1,
  assign: 0,
  catch: 1,
  coalesce: 2,
  or: 3,
  and: 4,
  compare: 5,
  range: 6,
  additive: 7,
  multiplicative: 8,
  shift: 9,
  unary: 10,
  power: 11,
  postfix: 12,
  primary: 13,
};

const BINARY = [
  ['??', PREC.coalesce, 'right'],
  ['||', PREC.or],
  ['&&', PREC.and],
  ['==', PREC.compare], ['!=', PREC.compare], ['<', PREC.compare],
  ['<=', PREC.compare], ['>', PREC.compare], ['>=', PREC.compare],
  ['..', PREC.range], ['..<', PREC.range],
  ['+', PREC.additive], ['-', PREC.additive], ['|', PREC.additive],
  ['^', PREC.additive], ['&+', PREC.additive], ['&-', PREC.additive],
  ['*', PREC.multiplicative], ['/', PREC.multiplicative], ['%', PREC.multiplicative],
  ['&', PREC.multiplicative], ['&*', PREC.multiplicative],
  ['<<', PREC.shift], ['>>', PREC.shift],
  ['**', PREC.power, 'right'],
];

const ASSIGN_OPS = [
  '=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<=', '>>=',
  '&+=', '&-=', '&*=', '**=', '??=',
];

const IDENT = /[_\p{XID_Start}][\p{XID_Continue}]*/;
const DIGITS = /[0-9](_?[0-9])*/.source;
const DECIMAL = `${DIGITS}(\\.${DIGITS})?([eE][+-]?${DIGITS})?`;

/**
 * An operator's tokens. >> and >>= are a > and, right after it, a > or a >=
 * (decision 24), so a type's Box<Box<int>> closes twice: the scanner makes
 * the first > where a shift may go.
 * @param {GrammarSymbols<string>} $
 * @param {string} op
 */
function operator($, op) {
  if (op === '>>') {
    return seq(alias($._shift_gt, '>'), token.immediate('>'));
  }
  if (op === '>>=') {
    return seq(alias($._shift_gt, '>'), token.immediate('>='));
  }
  return op;
}

/**
 * Comma-separated, at least one, an optional trailing comma.
 * @param {RuleOrLiteral} rule
 */
function commaSep1(rule) {
  return seq(rule, repeat(seq(',', rule)), optional(','));
}

/** @param {RuleOrLiteral} rule */
function commaSep(rule) {
  return optional(commaSep1(rule));
}

/**
 * Items separated by terminators (a newline or a ;), as a block's, a file's
 * or a type's body holds them.
 * @param {GrammarSymbols<string>} $
 * @param {RuleOrLiteral} item
 */
function items($, item) {
  return seq(repeat(choice($._terminator, seq(item, $._terminator))), optional(item));
}

/**
 * The expressions, twice: as they are, and in a control clause's head (an
 * if's condition, a for's sequence, a match's subject, a guard), where a {
 * starts the body instead of a call's trailing block (decision 4), and as
 * names the loop. A head's own rules are hidden and aliased to the names of
 * the others, so a tree reads the same either way.
 * @param {boolean} head
 */
function expressions(head) {
  const h = head ? '_head' : '';
  /** @param {string} name */
  const r = (name) => (head ? `_head_${name}` : name);
  /** @param {GrammarSymbols<string>} $ @param {string} name */
  const ref = ($, name) => (head ? alias($[`_head_${name}`], $[name]) : $[name]);
  /** @param {GrammarSymbols<string>} $ */
  const E = ($) => $[`_expression${h}`];
  /** @param {GrammarSymbols<string>} $ */
  const P = ($) => $[`_postfix${h}`];

  /** @type {Record<string, ($: GrammarSymbols<string>) => RuleOrLiteral>} */
  const rules = {
    [`_expression${h}`]: ($) => choice(
      ...(head ? [] : [$.lambda_expression, $.catch_expression]),
      ref($, 'binary_expression'),
      ref($, 'unary_expression'),
      ref($, 'try_expression'),
      ref($, 'load_expression'),
      ref($, 'reload_expression'),
      ref($, 'return_expression'),
      ref($, 'throw_expression'),
      $.break_expression,
      $.continue_expression,
      $.spawn_expression,
      P($),
    ),

    [`_postfix${h}`]: ($) => choice(
      $._primary,
      ref($, 'member_expression'),
      ref($, 'call_expression'),
      ref($, 'index_expression'),
      ref($, 'unwrap_expression'),
      ref($, 'generic_expression'),
    ),

    [r('binary_expression')]: ($) => choice(...BINARY.map(([op, p, assoc]) => {
      const rule = seq(field('left', E($)), field('operator', operator($, op)), field('right', E($)));
      const ruled = assoc === 'right' ? prec.right(p, rule) : prec.left(p, rule);
      // f<T>(x) over f < T > (x): comparisons don't chain.
      return p === PREC.compare ? prec.dynamic(-1, ruled) : ruled;
    })),

    [r('unary_expression')]: ($) => prec(PREC.unary, seq(
      field('operator', choice('!', '-', '~', 'await')),
      field('operand', E($)),
    )),

    [r('try_expression')]: ($) => prec(PREC.unary, seq(
      'try',
      optional(alias(token.immediate('?'), '?')),
      field('operand', E($)),
    )),

    // load f(x), load (a(), b()): what a component shows from a call.
    [r('load_expression')]: ($) => prec(PREC.unary, seq('load', field('operand', P($)))),

    // reload f(x), reload f: what loads ask again.
    [r('reload_expression')]: ($) => prec(PREC.unary, seq('reload', field('operand', P($)))),

    [r('return_expression')]: ($) => prec.right(seq('return', optional(field('value', E($))))),

    [r('throw_expression')]: ($) => prec.right(seq('throw', field('value', E($)))),

    [r('member_expression')]: ($) => prec(PREC.postfix, seq(
      field('object', P($)),
      choice('.', '?.'),
      field('property', $._member_name),
    )),

    // Box<int>.empty(), json.decode<Item>(s): a name's type arguments.
    [r('generic_expression')]: ($) => prec.dynamic(1, seq(
      field('value', choice($._ident, ref($, 'member_expression'))),
      field('type_arguments', $.type_arguments),
    )),

    [r('call_expression')]: ($) => {
      const fn = field('function', P($));
      if (head) {
        return choice(
          prec(PREC.postfix, seq(fn, field('arguments', $.arguments))),
          prec(PREC.postfix, seq(
            field('function', alias(choice('load', 'reload'), $.identifier)),
            field('arguments', alias($._immediate_arguments, $.arguments)),
          )),
        );
      }
      const name = optional(seq('as', field('name', $._ident)));
      const trailing = seq(field('body', $.block), repeat(field('handler', $.on_handler)));
      return choice(
        prec.right(PREC.postfix, seq(fn, field('arguments', $.arguments), name, optional(trailing))),
        // load(id), reload(): a call of a function so named, its ( right
        // after the name; load (api.user(id)) is the operator's.
        prec.right(PREC.postfix, seq(
          field('function', alias(choice('load', 'reload'), $.identifier)),
          field('arguments', alias($._immediate_arguments, $.arguments)),
          optional(trailing),
        )),
        // Row { … }, ui.Row as card { … }, animate { … }: an element, or a
        // call of a block alone.
        prec.right(PREC.postfix, seq(
          field('function', choice($._ident, $.member_expression, $.generic_expression)),
          name,
          trailing,
        )),
      );
    },

    [r('index_expression')]: ($) => prec(PREC.postfix, seq(
      field('object', P($)),
      '[',
      field('index', $._expression),
      optional(seq(',', field('label', alias('default', $.identifier)), ':', field('default', $._expression))),
      ']',
    )),

    [r('unwrap_expression')]: ($) => prec(PREC.postfix, seq(
      field('operand', P($)),
      token.immediate('!'),
    )),
  };
  return rules;
}

module.exports = grammar({
  name: 'asuka',

  word: $ => $.identifier,

  extras: $ => [/\s/, $.comment],

  externals: $ => [
    $._newline,
    $._multiline_string_content,
    $.raw_string_literal,
    $._shift_gt,
    $._error_sentinel,
  ],

  reserved: {
    global: _ => HARD_KEYWORDS,
    // After . or ?., in a label, a prop set's or a field's name: every
    // keyword is a name.
    name: _ => [],
  },

  supertypes: $ => [
    $._expression,
    $._type,
    $._pattern,
  ],

  inline: $ => [
    $._terminator,
    $._ident,
    $._name,
    $._member_name,
    $._postfix,
    $._postfix_head,
  ],

  conflicts: $ => [
    // x => …, or a name.
    [$._primary, $.lambda_parameter],
    // f<T>(x), or f < T > (x).
    [$._primary, $.generic_expression],
    [$._primary, $._head_generic_expression],
    [$._expression, $.generic_expression],
    [$._expression_head, $._head_generic_expression],
    // .added(shape: c): a label and its pattern, or a type test.
    [$.binding_pattern, $.field_pattern],
  ],

  rules: {
    source_file: $ => items($, choice($.import_declaration, $._declaration)),

    _terminator: $ => choice($._newline, ';'),

    comment: _ => token(choice(
      seq('//', /[^\n]*/),
      seq('/*', /[^*]*\*+([^/*][^*]*\*+)*/, '/'),
    )),

    // ------------------------------------------------------------------
    // Names

    identifier: _ => IDENT,

    // Where a declaration names something, and where an expression starts:
    // a soft keyword is a name (decision 10).
    _ident: $ => choice($.identifier, $._soft_keyword),

    // A soft keyword as a name: where its declaration or item may start
    // too, that wins (prop x, task { … }); elsewhere it's the name.
    _soft_keyword: $ => prec(-1, alias(choice(...SOFT_KEYWORDS, ...CONTEXTUAL), $.identifier)),

    // A label's, a prop set's, a field's, a member's: any keyword.
    _name: $ => choice(
      $.identifier,
      $._soft_keyword,
      prec(-1, alias(choice(...HARD_KEYWORDS), $.identifier)),
    ),

    _member_name: $ => choice(
      $._name,
      alias(token.immediate(/[0-9]+/), $.integer_literal),
    ),

    // ------------------------------------------------------------------
    // Imports

    import_declaration: $ => seq(
      'import',
      field('path', $.import_path),
      optional(seq('.{', commaSep(field('name', $._name)), '}')),
      optional(seq('as', field('alias', $._ident))),
    ),

    import_path: _ => token(seq(
      optional(choice('./', repeat1('../'))),
      optional('@'),
      /[_\p{XID_Start}][\p{XID_Continue}]*([\-.][\p{XID_Continue}]+)*/,
      repeat(seq('/', /[_\p{XID_Start}0-9][\p{XID_Continue}]*([\-.][\p{XID_Continue}]+)*/)),
    )),

    // ------------------------------------------------------------------
    // Declarations

    _declaration: $ => choice(
      $.component_declaration,
      $.screen_declaration,
      $.store_declaration,
      $.worker_declaration,
      $.shader_declaration,
      $.app_declaration,
      $.test_declaration,
      $.bench_declaration,
      $.preview_declaration,
      $.struct_declaration,
      $.enum_declaration,
      $.interface_declaration,
      $.extend_declaration,
      $.function_declaration,
      $.type_alias_declaration,
      $.extern_type_declaration,
      $.style_declaration,
      $.state_declaration,
      $.let_declaration,
    ),

    visibility: _ => choice('pub', 'private'),

    component_declaration: $ => seq(
      optional($.visibility),
      optional('native'),
      'component',
      field('name', $._ident),
      optional(field('type_parameters', $.type_parameters)),
      optional(field('parameters', $.parameters)),
      field('body', $.block),
    ),

    screen_declaration: $ => seq(
      optional($.visibility),
      'screen',
      field('name', $._ident),
      optional(field('parameters', $.parameters)),
      optional(seq('at', field('route', $.string_literal))),
      field('body', $.block),
    ),

    store_declaration: $ => seq(
      optional($.visibility),
      'store',
      field('name', $._ident),
      optional(field('type_parameters', $.type_parameters)),
      optional(field('parameters', $.parameters)),
      field('body', $.block),
    ),

    worker_declaration: $ => seq(
      optional($.visibility),
      'worker',
      field('name', $._ident),
      optional(field('parameters', $.parameters)),
      field('body', $.block),
    ),

    shader_declaration: $ => seq(
      optional($.visibility),
      'shader',
      field('name', $._ident),
      optional(field('parameters', $.parameters)),
      field('body', $.declaration_body),
    ),

    app_declaration: $ => seq('app', field('body', $.block)),

    test_declaration: $ => seq(
      'test',
      field('name', $._string),
      optional(field('parameters', $.parameters)),
      field('body', $.block),
    ),

    bench_declaration: $ => seq('bench', field('name', $._string), field('body', $.block)),

    // preview "Dark" (dark: true) { … }: a view shown in the editor.
    preview_declaration: $ => seq(
      'preview',
      field('name', $._string),
      optional(field('settings', $.arguments)),
      field('body', $.block),
    ),

    struct_declaration: $ => seq(
      optional($.visibility),
      optional('extern'),
      'struct',
      field('name', $._ident),
      optional(field('type_parameters', $.type_parameters)),
      optional(field('conformances', $.conformances)),
      field('body', $.declaration_body),
    ),

    enum_declaration: $ => seq(
      optional($.visibility),
      'enum',
      field('name', $._ident),
      optional(field('type_parameters', $.type_parameters)),
      optional(field('conformances', $.conformances)),
      field('body', $.enum_body),
    ),

    enum_body: $ => seq(
      '{',
      repeat(choice($._terminator, ',', seq($.enum_case, choice(',', $._terminator)), seq($._member, $._terminator))),
      optional(choice($.enum_case, $._member)),
      '}',
    ),

    enum_case: $ => seq(
      field('name', choice($._ident, alias(choice('none', 'reload', 'load', 'true', 'false', 'self'), $.identifier))),
      optional(seq('(', commaSep(alias($.case_parameter, $.parameter)), ')')),
    ),

    case_parameter: $ => seq(optional(seq(field('name', $._name), ':')), field('type', $._type)),

    interface_declaration: $ => seq(
      optional($.visibility),
      optional('native'),
      'interface',
      field('name', $._ident),
      optional(field('type_parameters', $.type_parameters)),
      field('body', $.declaration_body),
    ),

    extend_declaration: $ => seq(
      'extend',
      optional(field('type_parameters', $.type_parameters)),
      field('type', $._type),
      optional(field('conformances', $.conformances)),
      field('body', $.declaration_body),
    ),

    // A struct's, an interface's, an extension's, a shader's members.
    declaration_body: $ => seq('{', items($, $._member), '}'),

    _member: $ => choice(
      $.function_declaration,
      $.let_declaration,
      $.field_declaration,
    ),

    field_declaration: $ => seq(
      optional($.visibility),
      field('name', $._name),
      ':',
      field('type', $._type),
      optional(seq('=', field('value', $._expression))),
    ),

    conformances: $ => seq(':', commaSep1($._type)),

    function_declaration: $ => prec.right(seq(
      optional($.visibility),
      optional(choice(
        'static',
        'extern',
        seq('native', optional(seq('(', field('prefix', $.identifier), ')'))),
      )),
      optional('async'),
      optional('mut'),
      'fn',
      field('name', $._name),
      optional(field('type_parameters', $.type_parameters)),
      field('parameters', $.parameters),
      optional('throws'),
      optional(seq('->', field('result', $._type))),
      optional(choice(
        field('body', $.block),
        seq('=', field('value', $._expression)),
      )),
    )),

    parameters: $ => seq('(', commaSep($.parameter), ')'),

    parameter: $ => seq(
      field('name', $._name),
      optional(seq(':', field('type', $._type))),
      optional(seq('=', field('default', $._expression))),
    ),

    type_parameters: $ => seq('<', commaSep1($.type_parameter), '>'),

    type_parameter: $ => seq(
      field('name', $._ident),
      optional(seq(':', field('constraint', $._type), repeat(seq('&', field('constraint', $._type))))),
    ),

    type_alias_declaration: $ => seq(
      optional($.visibility),
      'type',
      field('name', $._ident),
      optional(field('type_parameters', $.type_parameters)),
      '=',
      field('type', $._type),
    ),

    // extern type Handle with release close
    extern_type_declaration: $ => seq(
      optional($.visibility),
      'extern',
      'type',
      field('name', $._ident),
      optional(seq('with', 'release', field('release', $.identifier))),
    ),

    style_declaration: $ => seq(
      optional($.visibility),
      'style',
      field('name', $._ident),
      optional(field('parameters', $.parameters)),
      field('body', $.block),
    ),

    state_declaration: $ => seq(
      optional($.visibility),
      'state',
      field('name', $._ident),
      optional(field('dependencies', $.dependencies)),
      optional(seq(':', field('type', $._type))),
      '=',
      field('value', $._expression),
    ),

    dependencies: $ => seq('(', commaSep($._expression), ')'),

    let_declaration: $ => prec.right(seq(
      optional($.visibility),
      choice('let', 'var'),
      field('pattern', $._pattern),
      optional(seq(':', field('type', $._type))),
      '=',
      field('value', $._expression),
      optional(seq('else', field('else', $.block))),
    )),

    // ------------------------------------------------------------------
    // Blocks and items

    block: $ => seq('{', items($, $._item), '}'),

    _item: $ => choice(
      $.prop_set,
      $.prop_declaration,
      $.event_declaration,
      $.state_declaration,
      $.let_declaration,
      $.use_declaration,
      $.provide_declaration,
      $.function_declaration,
      $.task_declaration,
      $.effect_declaration,
      $.cleanup_block,
      $.variant_declaration,
      $.style_declaration,
      $.style_application,
      $.on_handler,
      $.bind_item,
      $.for_statement,
      $.while_statement,
      $.do_statement,
      $.defer_statement,
      $.measure_statement,
      $.emit_statement,
      $.assignment,
      $._expression,
    ),

    // name: value, an element's prop.
    prop_set: $ => seq(field('name', $._name), ':', field('value', $._expression)),

    prop_declaration: $ => seq(
      optional($.visibility),
      'prop',
      field('name', $._ident),
      optional(seq(':', field('type', $._type))),
      optional(seq('=', field('default', $._expression))),
    ),

    event_declaration: $ => prec.right(seq(
      optional($.visibility),
      'event',
      field('name', $._ident),
      optional(field('parameters', $.parameters)),
    )),

    use_declaration: $ => seq('use', field('name', $._ident), ':', field('type', $._type)),

    provide_declaration: $ => seq('provide', field('value', $._expression)),

    task_declaration: $ => seq('task', optional(field('dependencies', $.dependencies)), field('body', $.block)),

    effect_declaration: $ => seq('effect', optional(field('dependencies', $.dependencies)), field('body', $.block)),

    cleanup_block: $ => seq('cleanup', field('body', $.block)),

    variant_declaration: $ => seq(optional($.visibility), 'variant', field('name', $._ident), '=', field('value', $._expression)),

    // style name, style pkg.name: a style applied.
    style_application: $ => prec.right(seq(
      'style',
      field('name', $._ident),
      repeat(seq('.', field('name', $._ident))),
    )),

    on_handler: $ => seq(
      'on',
      field('event', $._name),
      optional(seq('(', commaSep(alias($._ident, $.parameter)), ')')),
      field('body', $.block),
    ),

    bind_item: $ => seq('bind', field('name', $._name), ':', field('value', $._expression)),

    for_statement: $ => seq(
      'for',
      optional('await'),
      field('pattern', $._pattern),
      'in',
      field('sequence', $._expression_head),
      optional(seq('key', field('key', $._expression_head))),
      optional(seq('as', field('label', $._ident))),
      field('body', $.block),
    ),

    while_statement: $ => seq(
      'while',
      field('condition', $.condition),
      optional(seq('as', field('label', $._ident))),
      field('body', $.block),
    ),

    do_statement: $ => prec.right(seq('do', field('body', $.block), repeat1($.catch_clause))),

    defer_statement: $ => seq('defer', field('body', $.block)),

    measure_statement: $ => seq('measure', field('body', $.block)),

    emit_statement: $ => prec.right(seq('emit', field('event', $._name), optional(field('arguments', $.arguments)))),

    assignment: $ => prec.right(PREC.assign, seq(
      field('left', $._expression),
      field('operator', choice(...ASSIGN_OPS.map(op => operator($, op)))),
      field('right', $._expression),
    )),

    condition: $ => seq(
      choice($.let_condition, $._expression_head),
      repeat(seq(',', choice($.let_condition, $._expression_head))),
    ),

    let_condition: $ => seq(
      choice('let', 'var'),
      field('pattern', $._pattern),
      '=',
      field('value', $._expression_head),
    ),

    catch_clause: $ => seq(
      'catch',
      optional(field('pattern', $._pattern)),
      field('body', $.block),
    ),

    // ------------------------------------------------------------------
    // Expressions

    ...expressions(false),
    ...expressions(true),

    lambda_expression: $ => prec.right(PREC.lambda, seq(
      $._lambda_head,
      '=>',
      field('body', choice($.block, $._expression)),
    )),

    _lambda_head: $ => seq(
      optional('async'),
      field('parameters', choice(alias($._ident, $.lambda_parameter), $.lambda_parameters)),
    ),

    lambda_parameters: $ => seq('(', commaSep($.lambda_parameter), ')'),

    lambda_parameter: $ => seq(
      field('name', $._ident),
      optional(seq(':', field('type', $._type))),
      optional(seq('=', field('default', $._expression))),
    ),

    // x catch { … }: catch as a value (decision 23).
    catch_expression: $ => prec.right(PREC.catch, seq(field('value', $._expression), repeat1($.catch_clause))),

    break_expression: $ => prec.right(seq('break', optional(field('label', $._ident)))),

    continue_expression: $ => prec.right(seq('continue', optional(field('label', $._ident)))),

    spawn_expression: $ => seq('spawn', field('body', $.block)),

    arguments: $ => seq('(', commaSep($.argument), ')'),

    _immediate_arguments: $ => seq(token.immediate('('), commaSep($.argument), ')'),

    argument: $ => seq(
      optional(seq(field('label', $._name), ':')),
      field('value', $._expression),
    ),

    _primary: $ => choice(
      $._ident,
      $.self,
      $.integer_literal,
      $.float_literal,
      $.duration_literal,
      $.length_literal,
      $.color_literal,
      $.char_literal,
      $._string,
      $.boolean_literal,
      $.none,
      $.compile_time_constant,
      $.implicit_member_expression,
      $.parenthesized_expression,
      $.tuple_expression,
      $.array_expression,
      $.map_expression,
      $.if_expression,
      $.match_expression,
    ),

    self: _ => 'self',

    none: _ => 'none',

    boolean_literal: _ => choice('true', 'false'),

    // @platform, @debug
    compile_time_constant: $ => seq('@', alias(token.immediate(IDENT), $.identifier)),

    // .gray, .ready(v): an implicit member.
    implicit_member_expression: $ => prec(PREC.primary, seq('.', field('name', $._name))),

    parenthesized_expression: $ => seq('(', $._expression, ')'),

    tuple_expression: $ => choice(
      seq('(', ')'),
      seq('(', $._expression, ',', ')'),
      seq('(', $._expression, repeat1(seq(',', $._expression)), optional(','), ')'),
    ),

    array_expression: $ => seq('[', commaSep($._expression), ']'),

    map_expression: $ => choice(
      seq('[', ':', ']'),
      seq('[', commaSep1($.map_entry), ']'),
    ),

    map_entry: $ => seq(field('key', $._expression), ':', field('value', $._expression)),

    if_expression: $ => prec.right(seq(
      'if',
      field('condition', $.condition),
      field('consequence', $.block),
      optional(seq('else', field('alternative', choice($.if_expression, $.block)))),
    )),

    match_expression: $ => seq(
      'match',
      field('subject', $._expression_head),
      field('body', $.match_body),
    ),

    match_body: $ => seq(
      '{',
      repeat(choice($._terminator, ',', seq($.match_arm, choice(',', $._terminator)))),
      optional($.match_arm),
      '}',
    ),

    match_arm: $ => seq(
      field('pattern', $._pattern),
      optional(seq('if', field('guard', $._expression_head))),
      '=>',
      field('body', choice($.block, $._item)),
    ),

    // ------------------------------------------------------------------
    // Literals

    integer_literal: _ => token(choice(
      /0[xX][0-9a-fA-F](_?[0-9a-fA-F])*/,
      /0[bB][01](_?[01])*/,
      /0[oO][0-7](_?[0-7])*/,
      new RegExp(DIGITS),
    )),

    float_literal: _ => token(choice(
      new RegExp(`${DIGITS}\\.${DIGITS}([eE][+-]?${DIGITS})?`),
      new RegExp(`${DIGITS}[eE][+-]?${DIGITS}`),
    )),

    duration_literal: _ => token(new RegExp(`${DECIMAL}(ns|us|ms|s|min|h)`)),

    length_literal: _ => token(new RegExp(`${DECIMAL}(%|fr)`)),

    color_literal: _ => token(/#[0-9a-fA-F]{3,8}/),

    char_literal: _ => token(seq("'", choice(/[^'\\\n]/, /\\u\{[0-9a-fA-F]+\}/, /\\[^\n]/), "'")),

    _string: $ => choice($.string_literal, $.raw_string_literal),

    string_literal: $ => choice(
      seq(
        '"',
        repeat(choice(
          alias(token.immediate(prec(1, /[^"\\{\n]+/)), $.string_content),
          $.escape_sequence,
          $.interpolation,
        )),
        token.immediate('"'),
      ),
      seq(
        '"""',
        repeat(choice(
          alias($._multiline_string_content, $.string_content),
          $.escape_sequence,
          $.interpolation,
        )),
        '"""',
      ),
    ),

    escape_sequence: _ => token.immediate(seq('\\', choice(/u\{[0-9a-fA-F]+\}/, /[^\n]/))),

    interpolation: $ => seq(
      token.immediate('{'),
      field('value', $._expression),
      optional(field('format', $.format_specifier)),
      '}',
    ),

    // {x:>10.2}: a value's format, after its colon.
    format_specifier: _ => token(seq(':', /[^}"\n]*/)),

    // ------------------------------------------------------------------
    // Types

    _type: $ => choice(
      $.named_type,
      $.optional_type,
      $.array_type,
      $.map_type,
      $.tuple_type,
      $.parenthesized_type,
      $.function_type,
    ),

    // int, http.Response<T>
    named_type: $ => prec.right(seq(
      repeat(seq(field('package', alias($._ident, $.identifier)), '.')),
      field('name', alias($._ident, $.type_identifier)),
      optional(field('type_arguments', $.type_arguments)),
    )),

    type_arguments: $ => seq('<', commaSep1($._type), '>'),

    optional_type: $ => prec(2, seq(field('type', $._type), choice('?', '??'))),

    array_type: $ => seq('[', field('element', $._type), ']'),

    map_type: $ => seq('[', field('key', $._type), ':', field('value', $._type), ']'),

    parenthesized_type: $ => seq('(', $._type, ')'),

    tuple_type: $ => choice(
      seq('(', ')'),
      seq('(', $._type, ',', ')'),
      seq('(', $._type, repeat1(seq(',', $._type)), optional(','), ')'),
    ),

    function_type: $ => prec.right(1, seq(
      optional('async'),
      'fn',
      '(', commaSep($._type), ')',
      optional('throws'),
      optional(seq('->', field('result', $._type))),
    )),

    // ------------------------------------------------------------------
    // Patterns

    _pattern: $ => choice(
      $.or_pattern,
      $._primary_pattern,
    ),

    or_pattern: $ => prec.left(seq($._pattern, '|', $._primary_pattern)),

    _primary_pattern: $ => choice(
      $.wildcard_pattern,
      $.binding_pattern,
      $.typed_pattern,
      $.case_pattern,
      $.implicit_case_pattern,
      $.literal_pattern,
      $.tuple_pattern,
    ),

    wildcard_pattern: _ => '_',

    binding_pattern: $ => field('name', $._ident),

    // c: Circle, a type test.
    typed_pattern: $ => prec(-1, seq(
      field('pattern', choice($.binding_pattern, $.wildcard_pattern)),
      ':',
      field('type', $._type),
    )),

    // Shape.circle(r), Point(x: 0, y)
    case_pattern: $ => choice(
      seq(
        field('type', alias($._ident, $.identifier)),
        repeat1(seq('.', field('name', alias($._name, $.identifier)))),
        optional($.case_arguments),
      ),
      seq(field('name', alias($._ident, $.identifier)), $.case_arguments),
    ),

    // .ready(v)
    implicit_case_pattern: $ => seq('.', field('name', $._name), optional($.case_arguments)),

    case_arguments: $ => seq('(', commaSep(choice($.field_pattern, $._pattern)), ')'),

    field_pattern: $ => prec.dynamic(1, seq(field('label', $._name), ':', field('pattern', $._pattern))),

    literal_pattern: $ => choice(
      $.integer_literal,
      $.float_literal,
      $.duration_literal,
      $.length_literal,
      $.color_literal,
      $.char_literal,
      $._string,
      $.boolean_literal,
      $.none,
      seq('-', choice($.integer_literal, $.float_literal, $.duration_literal, $.length_literal)),
    ),

    tuple_pattern: $ => choice(
      seq('(', ')'),
      seq('(', $._pattern, ')'),
      seq('(', $._pattern, repeat1(seq(',', $._pattern)), optional(','), ')'),
    ),
  },
});
