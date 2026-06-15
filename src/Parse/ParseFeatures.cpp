// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements features directive parsing
 */

#include "ParserImpl.h"

#include "cangjie/AST/Clone.h"
#include "cangjie/Utils/Utils.h"

using namespace Cangjie;
using namespace Cangjie::AST;
using namespace Cangjie::Utils;

/**
 * featureId
 *     : (Ident | ContextIdent) (DOT (Ident | ContextIdent))*
 *     ;
 *
 * featuresSet
 *     : LCURL NL* 
 *     featureId NL* (COMMA NL* featureId NL*)* 
 *     RCURL
 *     ;
 *
 * featuresDirective
 *     : annotationList? FEATURES NL* 
 *     featuresSet
 *     end+
 *     ;
 */
void ParserImpl::ParseTopLvlFeatures(OwnedPtr<FeaturesDirective>& ftrDirective, PtrVector<Annotation>& annos)
{
    ftrDirective = MakeOwned<FeaturesDirective>();
    ftrDirective->featuresSet = MakeOwned<FeaturesSet>();

    Skip(TokenKind::FEATURES);
    ftrDirective->featuresPos = lastToken.Begin();
    ftrDirective->begin = lastToken.Begin();
    ftrDirective->end = lastToken.End();
    
    std::for_each(annos.begin(), annos.end(), [&, this](const OwnedPtr<Annotation>& anno){
        if (NotIn(anno->kind, {AnnotationKind::NON_PRODUCT})) {
            DiagUnexpectedAnnoOn(*anno, ftrDirective->featuresPos, anno->identifier, "features");
            ftrDirective->EnableAttr(Attribute::IS_BROKEN);
        }
    });
    ftrDirective->annotations = std::move(annos);

    if (!ftrDirective->annotations.empty()) {
        ftrDirective->begin = ftrDirective->annotations[0]->begin;
    }

    ParseFeatureDirective(ftrDirective);
    ftrDirective->end = ftrDirective->featuresSet->end;
}

void ParserImpl::ParseFeatureDirective(OwnedPtr<FeaturesDirective>& features)
{   
    bool isBroken{false};

    if (!Skip(TokenKind::LCURL)) {
        DiagExpectCharacter(lastToken.End(), "{");
        isBroken = true;
    } else {
        features->featuresSet->lCurlPos = lastToken.Begin();
        features->featuresSet->begin = lastToken.Begin();
    }

    ParseFeaturesSet(features->featuresSet);

    if (!Skip(TokenKind::RCURL)) {
        if (!features->featuresSet->lCurlPos.IsZero()) {
            DiagExpectedRightDelimiter("{", features->featuresSet->lCurlPos);
        } else {
            DiagExpectCharacter(lastToken.End(), "}");
        }
        isBroken = true;
    } else {
        features->featuresSet->rCurlPos = lastToken.Begin();
        features->featuresSet->end = lastToken.End();
    }

    if (isBroken) {
        features->EnableAttr(Attribute::HAS_BROKEN);
        features->featuresSet->EnableAttr(Attribute::IS_BROKEN);
    }
}

void ParserImpl::ParseFeaturesSet(OwnedPtr<FeaturesSet>& ftrSet)
{
    bool isBroken {false};
    while (!Seeing(TokenKind::END)) {
        if (Seeing(TokenKind::RCURL) || isBroken) {
            break;
        }

        if (Seeing(TokenKind::IDENTIFIER)) {
            if (lastToken.kind == TokenKind::IDENTIFIER) {
                DiagExpectCharacter(lastToken.End(), "'.' or ','");
                ConsumeUntilAny({TokenKind::NL, TokenKind::COMMA, TokenKind::RCURL}, false);
                isBroken = true;
            }else {
                ParseFeatureId(ftrSet);
            }

            if (Skip(TokenKind::COMMA)) {
                ftrSet->commaPoses.emplace_back(lastToken.Begin());
            }

        } else {
            DiagExpectedIdentifier(MakeRange(lastToken.Begin(), lastToken.End()),
                "an identifier", "after this", false);
            ConsumeUntilAny({TokenKind::NL, TokenKind::COMMA, TokenKind::RCURL}, false);
            isBroken = true;
        }
    }
    ftrSet->end = lastToken.End();
    if (isBroken) {
        ftrSet->EnableAttr(Attribute::HAS_BROKEN);
    }
}

void ParserImpl::ParseFeatureId(OwnedPtr<FeaturesSet>& featuresSet)
{
    FeatureId content;
    bool firstIter{true};
    bool isBroken{false};
    while (Skip(TokenKind::IDENTIFIER) || Skip(TokenKind::DOT)) {
        if (firstIter) {
            content.begin = lastToken.Begin();
            content.end = lastToken.End();
            firstIter = false;
        }
        std::string current = lastToken.Value();
        if (lastToken.kind == TokenKind::IDENTIFIER) {
            content.identifiers.emplace_back(Identifier(current, lastToken.Begin(), lastToken.End()));
        } else {
            content.dotPoses.emplace_back(lastToken.Begin());
        }
        content.end = lastToken.End();
        if (Seeing(lastToken.kind)) {
            isBroken = true;
        }
        if (IsRawIdentifier(current)) {
            DiagRawIdentifierNotAllowed(current);
            isBroken = true;
        }
        if (isBroken) {
            break;
        }
    }
    if (lastToken.kind != TokenKind::IDENTIFIER) { 
        DiagExpectedIdentifier(MakeRange(content.begin, content.end),
            "an identifier", "after this", false);
        isBroken = true;
    }

    if (isBroken) {
        content.EnableAttr(Attribute::IS_BROKEN);
    }

    if (!content.identifiers.empty()) {
        featuresSet->content.emplace_back(content);
    }
}
