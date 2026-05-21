// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the symbol table printing function.
 */

#ifndef CANGJIE_AST_PRINTSYMBOLTABLE_H
#define CANGJIE_AST_PRINTSYMBOLTABLE_H

#include "cangjie/Frontend/CompilerInstance.h"

namespace Cangjie {
/**
 * Print the symbol tables of the compiler instance.
 *
 * The output is formatted as JSON and the following schema is required:
 *
 * {
 *   "packages": [
 *     {
 *       "name": <name of the package>,
 *       "files": [
 *         <path of the file>
 *       ]
 *     }
 *   ],
 *   "files": [
 *     {
 *       "path": <path of the file>,
 *       "symbols": [
 *         {
 *           "astKind": <AST kind of the symbol>,
 *           "name": <name of the symbol>,
 *           if astKind = package_spec:
 *           "packageName": <name of package>,
 *           "packagePos": {
 *             "line": <integer>,
 *             "column": <integer>
 *           },
 *           "packageNamePos": {
 *             "line": <integer>,
 *             "column": <integer>
 *           },
 *           endif
 *           if astKind = import_spec:
 *           if has from keyword:
 *           "fromPos": {
 *             "line": <integer>,
 *             "column": <integer>
 *           },
 *           "moduleName": <name of module>,
 *           "modulePos": {
 *             "line": <integer>,
 *             "column": <integer>
 *           },
 *           endif
 *           "importPos": {
 *             "line": <integer>,
 *             "column": <integer>
 *           },
 *           "packageName": <name of package>,
 *           "PackageNamePos": {
 *             "line": <integer>,
 *             "column": <integer>
 *           },
 *           "importedItemName": <name of imported item>,
 *           "importedItemNamePos": {
 *             "line": <integer>,
 *             "column": <integer>
 *           },
 *           if has as keyword:
 *           "asPos": {
 *             "line": <integer>,
 *             "column": <integer>
 *           },
 *           "asIdentifier": <name of asIdentifier>,
 *           "asIdentifierPos": {
 *             "line": <integer>,
 *             "column": <integer>
 *           },
 *           endif
 *           endif
 *           if astKind = *decl
 *           "identifier": <name of identifier>,
 *           "identifierPos": {
 *             "line": <integer>,
 *             "column": <integer>
 *           },
 *           endif
 *           "begin": {
 *             "line": <integer>,
 *             "column": <integer>
 *           },
 *           "end": {
 *             "line": <integer>,
 *             "column": <integer>
 *           }
 *         }
 *       ]
 *     }
 *   ]
 * }
 */
void PrintSymbolTable(CompilerInstance& ci);
} // namespace Cangjie

#endif
