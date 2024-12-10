#pragma once

#include <fstream>
#include <functional>

#include "ast/scopes/registers.hpp"
#include "lexer.ipp"
#include "ast/nodes/nodes.h"
#include "dbg/errors.hpp"
#include "dbg/utils.hpp"
#include "ast/litteralTypes.hpp"

namespace parser
{

namespace { using namespace lexer; }

class Parser
{
public:
  Parser(Lexer &&lexer): _lexer{std::move(lexer)} {}
  Parser(std::ifstream &&inputFile): _lexer(inputFile) {}

  ast::TranslationUnit parseTranslationUnit() {
    std::vector<ast::Function> funcList{};
    std::vector<ast::Class> classList{};
    nextToken();
    while (_currentToken.type != TT_END)
    {
      if (_currentToken.type == TT_K_CLASS)
        classList.emplace_back(parseClass());
      else
        funcList.emplace_back(parseFunction());
    }
    match(TT_END);

    return ast::TranslationUnit(std::move(funcList), std::move(classList));
  }

private:
  inline std::string_view getRawUntil(char breaker)
  {
    std::string_view raw = _lexer.getRawUntil(breaker);
    _currentToken = _lexer.nextToken();
    return raw;
  }

  inline void nextToken()
  {
    _currentToken = _lexer.nextToken();

    // for debug purposes
    // LOG_INLINE(_currentToken.value << " ");
    // static size_t endCount = 0;
    // if (_currentToken.type == TT_END && ++endCount > 10)
    // {
    //   USER_THROW("END TOKEN SEEN TOO MANY TIMES");
    // }
  }

  std::string_view match(TokenType type) {
    USER_ASSERT(_currentToken.type == type, "Unexpected token type=[" << _currentToken.type << "] for value=[" << _currentToken.value << "] expected=[" << type << "]", _currentToken.position);
    std::string_view cur = _currentToken.value;
    nextToken();
    return cur;
  }

  enum TrailingSeparator
  {
    TRAILING_FORBIDDEN,
    TRAILING_OPTIONAL,
    TRAILING_REQUIRED,
  };

  template<typename T, TokenType ttSeparator, TrailingSeparator trailingMode, TokenType ttBreaker = TT_NONE>
  inline std::vector<T> parseSeparatedList(std::function<T()> parseElement) {
    std::vector<T> elementList;

    while (ttBreaker == TT_NONE || _currentToken.type != ttBreaker)
    {
      elementList.push_back(parseElement());

      if constexpr (ttSeparator != TT_NONE)
      {
        if (_currentToken.type == ttSeparator) match(ttSeparator);
        else if constexpr (trailingMode == TRAILING_REQUIRED) USER_THROW("Expected trailing " << ttSeparator, _currentToken.position);
        else break;
      }

      if constexpr (ttBreaker != TT_NONE && trailingMode == TRAILING_FORBIDDEN)
      {
        USER_ASSERT(_currentToken.type != ttBreaker, "Found trailing " << ttSeparator << " in list, expected new element", _currentToken.position);
      }
    }

    return std::move(elementList);
  }

  void matchIdent(std::string_view ident)
  {
    std::string_view cur = match(TT_IDENT);
    USER_ASSERT(cur == ident, "Specific expected ident was not matched. Expected=[" << ident << "] got=[" << cur << "]");
  }

  std::string_view parsePureType()
  {
    /* if (_currentToken.type == TT_K_INT) return match(TT_K_INT); */
    /* if (_currentToken.type == TT_K_VOID) return match(TT_K_VOID); */
    /* ... */
#define X(token, str) \
    if (_currentToken.type == token) return match(token);
    PURE_TYPES_TOKEN_LIST
#undef X

    return match(TT_IDENT);
  }

  ast::Type parseType()
  {
    std::string_view pureType = parsePureType();
    int pointerDepth = 0;
    while (_currentToken.type == TT_STAR)
    {
      match(TT_STAR);
      pointerDepth++;
    }

    return ast::Type(pureType, pointerDepth);
  }

  ast::Function parseFunction()
  {
    ast::Type returnType = parseType();
    std::string_view name = match(TT_IDENT);
    ast::FunctionParameterList parametersNode = parseFunctionParams();
    ast::InstructionList body = parseCodeBlock();
    return ast::Function(std::move(returnType), name, std::move(parametersNode), std::move(body));
  }

  ast::Class parseClass() {
    match(TT_K_CLASS);
    std::string_view name = match(TT_IDENT);
    match(TT_LCURL);

    ast::Class::MethodList methods;
    ast::Class::AttributeList attributes;

    while (_currentToken.type != TT_RCURL) {
      ast::AccessSpecifier attribute_specifier(ast::Visibility::Public);
      ast::Type type = parseType();
      std::string_view name = match(TT_IDENT);
      if (_currentToken.type == TT_LPAR) {
        ast::FunctionParameterList parametersNode = parseFunctionParams();
        ast::InstructionList body = parseCodeBlock();
        ast::Method method(std::move(type), name, std::move(parametersNode), std::move(body));
        methods.emplace_back(std::move(method), std::move(attribute_specifier));
      }
      else {
        ast::Attribute attribute(std::move(type), name);
        attributes.emplace_back(std::move(attribute), std::move(attribute_specifier));
        match(TT_SEMI);
      }
    }
    match(TT_RCURL);
    match(TT_SEMI);

    return ast::Class(name, std::move(attributes), std::move(methods));
  }

  ast::FunctionParameter parseSingleParam()
  {
    auto type = parseType();
    std::string_view name{};
    if (_currentToken.type == TT_IDENT) name = match(TT_IDENT);

    return ast::FunctionParameter(std::move(type), name);
  }

  ast::FunctionParameterList parseFunctionParams()
  {
    match(TT_LPAR);
    auto functionParams = parseSeparatedList<ast::FunctionParameter, TT_COMMA, TRAILING_FORBIDDEN, TT_RPAR>([this]() { return parseSingleParam(); });
    match(TT_RPAR);
    return ast::FunctionParameterList(std::move(functionParams));
  }

  scopes::Register parseRegisterName()
  {
    std::string_view raw = parseRawSingleStringLiteral();
    USER_ASSERT(raw[0] == '=', "Only ={register} identifiers are supported", _currentToken.position);
    return scopes::strToReg(raw.substr(1));
  }

  // "a" "b" won't work, a single double quoted value is possible here, no escape characters are replaced
  std::string_view parseRawSingleStringLiteral()
  {
    USER_ASSERT(_currentToken.type == lexer::TT_DOUBLE_QUOTE, "Expected double quote for literal", _currentToken.position);
    std::string_view raw = getRawUntil('"');
    match(lexer::TT_DOUBLE_QUOTE);
    return raw;
  }

  // "a" "\tb" is a valid string literal with concat, \t will be replaced by a tab
  ast::StringLiteral parseStringLiteral()
  {
    std::vector<std::string_view> literals;
    size_t totalSize = 0;
    while (_currentToken.type == lexer::TT_DOUBLE_QUOTE)
    {
      std::string_view singleLiteral = parseRawSingleStringLiteral();
      totalSize += singleLiteral.size();
      literals.push_back(std::move(singleLiteral));
    }

    if (literals.empty()) USER_THROW("Expected a valid string literal", _currentToken.position);

    std::string content;
    content.reserve(totalSize+1);
    for (auto &lit: literals) content += lexer::Lexer::replaceEscapes(lit);

    return ast::StringLiteral(content);
  }

  ast::NumberLiteral parseNumberLiteral()
  {
    auto numberView = match(TT_NUMBER);
    ast::NumberLitteralUnderlyingType number = utils::readNumber<ast::NumberLitteralUnderlyingType>(numberView);
    return ast::NumberLiteral(number);
  }

  ast::Expression parseExpression()
  {
    if (_currentToken.type == TT_IDENT)
    {
      std::string_view ident = match(TT_IDENT);
      // if (_currentToken.type == TT_LPAR)
      // {
      //   ast::FunctionCall functionCall = parseFunctionCall();
      //   return ast::Expression(std::move(functionCall));
      // }
      // else
      // {
      //   return ast::Expression(ast::Variable(std::move(ident)));
      // }
      return ast::Expression(ast::Variable(std::move(ident)));
    }

    auto numberLiteral = parseNumberLiteral();
    return ast::Expression(std::move(numberLiteral));
  }

  ast::ReturnStatement parseReturnStatement()
  {
    match(TT_K_RETURN);
    auto expression = parseExpression();
    return ast::ReturnStatement(std::move(expression));
  }

  ast::InlineAsmStatement::BindingRequest parseBindingRequest()
  {
    scopes::Register registerTo = parseRegisterName();
    match(lexer::TT_LPAR);
    auto ident = match(TT_IDENT);
    match(lexer::TT_RPAR);

    // plain old data
    return {
      .registerTo=registerTo,
      .varIdentifier=std::string(ident),
    };
  }

  ast::InlineAsmStatement parseInlineAsmStatement()
  {
    match(lexer::TT_K_ASM);
    match(lexer::TT_LPAR);

    ast::StringLiteral asmBlock = parseStringLiteral();

    std::vector<ast::InlineAsmStatement::BindingRequest> requests;
    if (_currentToken.type == lexer::TT_COLON)
    {
      match(TT_COLON);
      requests = parseSeparatedList<ast::InlineAsmStatement::BindingRequest, lexer::TT_COMMA, TRAILING_OPTIONAL, lexer::TT_RPAR>([this](){ return parseBindingRequest(); });
    }

    match(lexer::TT_RPAR);

    return ast::InlineAsmStatement(std::move(asmBlock), std::move(requests));
  }

  ast::Instruction parseSingleInstruction()
  {
    switch ( _currentToken.type )
    {
      case lexer::TT_K_RETURN:
        return ast::Instruction(parseReturnStatement());
      case lexer::TT_K_ASM:
        return ast::Instruction(parseInlineAsmStatement());

      /* case lexer::TT_K_INT: return ast::Instruction(...); */
      /* case lexer::TT_K_VOID: return ast::Instruction(...); */
      /* ... */
      #define X(token, str) \
        case token: return ast::Instruction(parseDeclaration());
        PURE_TYPES_TOKEN_LIST
      #undef X
      default:
        USER_THROW("Unexpected token while parsing instruction [" << _currentToken.type << "]", _currentToken.position);
    }
  }

  ast::Declaration parseDeclaration()
  {
    ast::Type type = parseType();
    std::string_view name = match(TT_IDENT);
    if (_currentToken.type == TT_EQUAL)
    {
      match(TT_EQUAL);
      auto expression = parseExpression();
      return ast::Declaration(std::move(type), ast::Variable(std::move(name)), std::move(expression));
    }
    else 
    {
      return ast::Declaration(std::move(type), ast::Variable(std::move(name)));
    }
  }

  ast::FunctionCall parseFunctionCall()
  {
    std::string_view name = match(TT_IDENT);
    match(TT_LPAR);
    std::vector<ast::Expression> arguments;
    while (_currentToken.type != TT_RPAR)
    {
      arguments.push_back(parseExpression());
      // TODO: how do we know the expression will end?
      match(TT_COMMA);
    }
    match(TT_RPAR);
    return ast::FunctionCall(name, std::move(arguments));
  }

  ast::InstructionList parseCodeBlock()
  {
    match(TT_LCURL);
    auto instructions = parseSeparatedList<ast::Instruction, TT_SEMI, TRAILING_REQUIRED, TT_RCURL>([this]() { return parseSingleInstruction(); });
    match(TT_RCURL);
    return ast::InstructionList(std::move(instructions));
  }

private:
  Lexer _lexer;
  Token _currentToken;
};

} /* namespace parser */
