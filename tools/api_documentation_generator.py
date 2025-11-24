#!/usr/bin/env python3
"""
AutoWatering API Documentation Generator

This script parses C header files and generates comprehensive API documentation
for the AutoWatering irrigation system. It extracts function signatures, data
structures, enums, and creates cross-reference mappings.

Usage:
    python api_documentation_generator.py [options]

Options:
    --source-dir DIR    Source directory to scan (default: src/)
    --output-dir DIR    Output directory for documentation (default: docs/api/)
    --format FORMAT     Output format: markdown, json, html (default: markdown)
    --verbose          Enable verbose logging
"""

import os
import re
import json
import argparse
import logging
from typing import Dict, List, Optional, Tuple, Any
from dataclasses import dataclass, asdict
from pathlib import Path

# Configure logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)

@dataclass
class FunctionInfo:
    """Information about a C function"""
    name: str
    return_type: str
    parameters: List[Dict[str, str]]
    description: str
    file_path: str
    line_number: int
    is_static: bool = False
    is_inline: bool = False
    
@dataclass
class StructInfo:
    """Information about a C structure"""
    name: str
    fields: List[Dict[str, Any]]
    description: str
    file_path: str
    line_number: int
    is_packed: bool = False
    size_bytes: Optional[int] = None

@dataclass
class EnumInfo:
    """Information about a C enum"""
    name: str
    values: List[Dict[str, Any]]
    description: str
    file_path: str
    line_number: int

@dataclass
class MacroInfo:
    """Information about a C macro"""
    name: str
    value: str
    description: str
    file_path: str
    line_number: int

@dataclass
class HeaderFileInfo:
    """Complete information about a header file"""
    file_path: str
    functions: List[FunctionInfo]
    structures: List[StructInfo]
    enums: List[EnumInfo]
    macros: List[MacroInfo]
    includes: List[str]
    description: str

class CHeaderParser:
    """Parser for C header files"""
    
    def __init__(self):
        self.function_pattern = re.compile(
            r'(?:/\*\*(.*?)\*/\s*)?'  # Optional Doxygen comment
            r'(?:(static|inline)\s+)?'  # Optional static/inline
            r'(\w+(?:\s*\*)*)\s+'  # Return type
            r'(\w+)\s*'  # Function name
            r'\((.*?)\)\s*;',  # Parameters
            re.DOTALL | re.MULTILINE
        )
        
        self.struct_pattern = re.compile(
            r'(?:/\*\*(.*?)\*/\s*)?'  # Optional Doxygen comment
            r'(?:typedef\s+)?struct\s+(\w+)?\s*\{'  # Struct declaration
            r'(.*?)'  # Struct body
            r'\}\s*(?:(\w+))?\s*(?:__packed)?\s*;',  # Optional typedef name and packed
            re.DOTALL | re.MULTILINE
        )
        
        self.enum_pattern = re.compile(
            r'(?:/\*\*(.*?)\*/\s*)?'  # Optional Doxygen comment
            r'typedef\s+enum\s*\{'  # Enum declaration
            r'(.*?)'  # Enum body
            r'\}\s*(\w+)\s*;',  # Typedef name
            re.DOTALL | re.MULTILINE
        )
        
        self.macro_pattern = re.compile(
            r'(?:/\*\*(.*?)\*/\s*)?'  # Optional Doxygen comment
            r'#define\s+(\w+)(?:\([^)]*\))?\s+(.*?)(?:\n|$)',  # Macro definition
            re.MULTILINE
        )
        
        self.include_pattern = re.compile(r'#include\s+[<"]([^>"]+)[>"]')
        
    def parse_file(self, file_path: str) -> HeaderFileInfo:
        """Parse a single header file"""
        logger.info(f"Parsing header file: {file_path}")
        
        try:
            with open(file_path, 'r', encoding='utf-8') as f:
                content = f.read()
        except Exception as e:
            logger.error(f"Failed to read file {file_path}: {e}")
            return HeaderFileInfo(file_path, [], [], [], [], [], "")
        
        # Extract file description from header comment
        description = self._extract_file_description(content)
        
        # Parse different elements
        functions = self._parse_functions(content, file_path)
        structures = self._parse_structures(content, file_path)
        enums = self._parse_enums(content, file_path)
        macros = self._parse_macros(content, file_path)
        includes = self._parse_includes(content)
        
        return HeaderFileInfo(
            file_path=file_path,
            functions=functions,
            structures=structures,
            enums=enums,
            macros=macros,
            includes=includes,
            description=description
        )
    
    def _extract_file_description(self, content: str) -> str:
        """Extract file description from header comment"""
        # Look for file-level Doxygen comment
        file_comment_pattern = re.compile(
            r'/\*\*\s*\n\s*\*\s*@file[^\n]*\n\s*\*\s*@brief\s+(.*?)\n(?:\s*\*.*?\n)*?\s*\*/',
            re.DOTALL
        )
        
        match = file_comment_pattern.search(content)
        if match:
            return match.group(1).strip()
        
        # Fallback: look for any header comment
        header_comment_pattern = re.compile(r'/\*\*(.*?)\*/', re.DOTALL)
        match = header_comment_pattern.search(content)
        if match:
            comment = match.group(1)
            # Extract brief description
            brief_match = re.search(r'@brief\s+(.*?)(?:\n|\*)', comment)
            if brief_match:
                return brief_match.group(1).strip()
        
        return ""
    
    def _parse_functions(self, content: str, file_path: str) -> List[FunctionInfo]:
        """Parse function declarations"""
        functions = []
        
        for match in self.function_pattern.finditer(content):
            doc_comment = match.group(1) or ""
            modifiers = match.group(2) or ""
            return_type = match.group(3).strip()
            func_name = match.group(4)
            params_str = match.group(5)
            
            # Skip if this looks like a variable declaration
            if not params_str or '(' not in match.group(0):
                continue
                
            # Parse parameters
            parameters = self._parse_parameters(params_str)
            
            # Extract description from doc comment
            description = self._extract_description_from_comment(doc_comment)
            
            # Get line number
            line_number = content[:match.start()].count('\n') + 1
            
            functions.append(FunctionInfo(
                name=func_name,
                return_type=return_type,
                parameters=parameters,
                description=description,
                file_path=file_path,
                line_number=line_number,
                is_static="static" in modifiers,
                is_inline="inline" in modifiers
            ))
        
        return functions
    
    def _parse_parameters(self, params_str: str) -> List[Dict[str, str]]:
        """Parse function parameters"""
        if not params_str.strip() or params_str.strip() == "void":
            return []
        
        parameters = []
        # Split by comma, but be careful of function pointers
        param_parts = []
        paren_depth = 0
        current_param = ""
        
        for char in params_str:
            if char == '(':
                paren_depth += 1
            elif char == ')':
                paren_depth -= 1
            elif char == ',' and paren_depth == 0:
                param_parts.append(current_param.strip())
                current_param = ""
                continue
            current_param += char
        
        if current_param.strip():
            param_parts.append(current_param.strip())
        
        for param in param_parts:
            param = param.strip()
            if not param:
                continue
                
            # Parse parameter type and name
            parts = param.split()
            if len(parts) >= 2:
                param_name = parts[-1].lstrip('*')
                param_type = ' '.join(parts[:-1])
            else:
                param_name = ""
                param_type = param
            
            parameters.append({
                'name': param_name,
                'type': param_type,
                'description': ""  # Could be extracted from doc comments
            })
        
        return parameters
    
    def _parse_structures(self, content: str, file_path: str) -> List[StructInfo]:
        """Parse structure definitions"""
        structures = []
        
        for match in self.struct_pattern.finditer(content):
            doc_comment = match.group(1) or ""
            struct_name = match.group(2) or match.group(4) or "anonymous"
            struct_body = match.group(3)
            
            # Parse struct fields
            fields = self._parse_struct_fields(struct_body)
            
            # Extract description
            description = self._extract_description_from_comment(doc_comment)
            
            # Get line number
            line_number = content[:match.start()].count('\n') + 1
            
            # Check if packed
            is_packed = "__packed" in match.group(0)
            
            structures.append(StructInfo(
                name=struct_name,
                fields=fields,
                description=description,
                file_path=file_path,
                line_number=line_number,
                is_packed=is_packed
            ))
        
        return structures
    
    def _parse_struct_fields(self, struct_body: str) -> List[Dict[str, Any]]:
        """Parse structure fields"""
        fields = []
        
        # Remove comments first
        struct_body = re.sub(r'/\*.*?\*/', '', struct_body, flags=re.DOTALL)
        struct_body = re.sub(r'//.*?$', '', struct_body, flags=re.MULTILINE)
        
        # Split into lines and parse each field
        lines = struct_body.split('\n')
        for line in lines:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            
            # Handle union/struct within struct
            if line.startswith('union') or line.startswith('struct'):
                # This is a nested structure - simplified handling
                continue
            
            # Parse field declaration
            if ';' in line:
                field_decl = line.split(';')[0].strip()
                parts = field_decl.split()
                
                if len(parts) >= 2:
                    field_name = parts[-1].lstrip('*').rstrip('[]')
                    field_type = ' '.join(parts[:-1])
                    
                    # Extract array size if present
                    array_size = None
                    if '[' in parts[-1] and ']' in parts[-1]:
                        array_match = re.search(r'\[(\d+)\]', parts[-1])
                        if array_match:
                            array_size = int(array_match.group(1))
                    
                    fields.append({
                        'name': field_name,
                        'type': field_type,
                        'array_size': array_size,
                        'description': ""  # Could extract from inline comments
                    })
        
        return fields
    
    def _parse_enums(self, content: str, file_path: str) -> List[EnumInfo]:
        """Parse enum definitions"""
        enums = []
        
        for match in self.enum_pattern.finditer(content):
            doc_comment = match.group(1) or ""
            enum_body = match.group(2)
            enum_name = match.group(3)
            
            # Parse enum values
            values = self._parse_enum_values(enum_body)
            
            # Extract description
            description = self._extract_description_from_comment(doc_comment)
            
            # Get line number
            line_number = content[:match.start()].count('\n') + 1
            
            enums.append(EnumInfo(
                name=enum_name,
                values=values,
                description=description,
                file_path=file_path,
                line_number=line_number
            ))
        
        return enums
    
    def _parse_enum_values(self, enum_body: str) -> List[Dict[str, Any]]:
        """Parse enum values"""
        values = []
        
        # Remove comments
        enum_body = re.sub(r'/\*.*?\*/', '', enum_body, flags=re.DOTALL)
        enum_body = re.sub(r'//.*?$', '', enum_body, flags=re.MULTILINE)
        
        # Split by comma
        value_parts = enum_body.split(',')
        
        for i, part in enumerate(value_parts):
            part = part.strip()
            if not part:
                continue
            
            # Parse value name and optional explicit value
            if '=' in part:
                name, value = part.split('=', 1)
                name = name.strip()
                value = value.strip()
            else:
                name = part.strip()
                value = str(i)  # Default enum value
            
            # Extract inline comment as description
            description = ""
            if '/**<' in name:
                desc_match = re.search(r'/\*\*<\s*(.*?)\s*\*/', name)
                if desc_match:
                    description = desc_match.group(1)
                    name = re.sub(r'\s*/\*\*<.*?\*/', '', name).strip()
            
            values.append({
                'name': name,
                'value': value,
                'description': description
            })
        
        return values
    
    def _parse_macros(self, content: str, file_path: str) -> List[MacroInfo]:
        """Parse macro definitions"""
        macros = []
        
        for match in self.macro_pattern.finditer(content):
            doc_comment = match.group(1) or ""
            macro_name = match.group(2)
            macro_value = match.group(3).strip()
            
            # Skip system includes and complex macros
            if macro_name.startswith('_') or len(macro_value) > 100:
                continue
            
            # Extract description
            description = self._extract_description_from_comment(doc_comment)
            
            # Get line number
            line_number = content[:match.start()].count('\n') + 1
            
            macros.append(MacroInfo(
                name=macro_name,
                value=macro_value,
                description=description,
                file_path=file_path,
                line_number=line_number
            ))
        
        return macros
    
    def _parse_includes(self, content: str) -> List[str]:
        """Parse include statements"""
        includes = []
        for match in self.include_pattern.finditer(content):
            includes.append(match.group(1))
        return includes
    
    def _extract_description_from_comment(self, comment: str) -> str:
        """Extract description from Doxygen comment"""
        if not comment:
            return ""
        
        # Remove comment markers
        comment = re.sub(r'^\s*\*\s?', '', comment, flags=re.MULTILINE)
        comment = comment.strip()
        
        # Extract brief description
        brief_match = re.search(r'@brief\s+(.*?)(?:\n|@)', comment, re.DOTALL)
        if brief_match:
            return brief_match.group(1).strip()
        
        # Fallback: use first line
        lines = comment.split('\n')
        if lines:
            return lines[0].strip()
        
        return ""

class APIDocumentationGenerator:
    """Generates API documentation from parsed header files"""
    
    def __init__(self, output_format: str = "markdown"):
        self.output_format = output_format
        self.parser = CHeaderParser()
    
    def generate_documentation(self, source_dir: str, output_dir: str) -> None:
        """Generate complete API documentation"""
        logger.info(f"Generating API documentation from {source_dir} to {output_dir}")
        
        # Create output directory
        Path(output_dir).mkdir(parents=True, exist_ok=True)
        
        # Find all header files
        header_files = self._find_header_files(source_dir)
        
        # Parse all header files
        parsed_files = []
        for header_file in header_files:
            parsed_file = self.parser.parse_file(header_file)
            parsed_files.append(parsed_file)
        
        # Generate documentation files
        self._generate_overview(parsed_files, output_dir)
        self._generate_function_reference(parsed_files, output_dir)
        self._generate_data_structures_reference(parsed_files, output_dir)
        self._generate_cross_references(parsed_files, output_dir)
        
        # Generate individual file documentation
        for parsed_file in parsed_files:
            self._generate_file_documentation(parsed_file, output_dir)
        
        logger.info("API documentation generation completed")
    
    def _find_header_files(self, source_dir: str) -> List[str]:
        """Find all header files in source directory"""
        header_files = []
        
        for root, dirs, files in os.walk(source_dir):
            for file in files:
                if file.endswith('.h'):
                    header_files.append(os.path.join(root, file))
        
        return sorted(header_files)
    
    def _generate_overview(self, parsed_files: List[HeaderFileInfo], output_dir: str) -> None:
        """Generate API overview documentation"""
        output_file = os.path.join(output_dir, "README.md")
        
        with open(output_file, 'w') as f:
            f.write("# AutoWatering API Reference\n\n")
            f.write("This document provides comprehensive API documentation for the AutoWatering irrigation system.\n\n")
            
            # Statistics
            total_functions = sum(len(pf.functions) for pf in parsed_files)
            total_structures = sum(len(pf.structures) for pf in parsed_files)
            total_enums = sum(len(pf.enums) for pf in parsed_files)
            
            f.write("## API Statistics\n\n")
            f.write(f"- **Header Files**: {len(parsed_files)}\n")
            f.write(f"- **Functions**: {total_functions}\n")
            f.write(f"- **Data Structures**: {total_structures}\n")
            f.write(f"- **Enumerations**: {total_enums}\n\n")
            
            # File index
            f.write("## Header Files\n\n")
            for parsed_file in parsed_files:
                filename = os.path.basename(parsed_file.file_path)
                f.write(f"### [{filename}]({filename.replace('.h', '.md')})\n")
                if parsed_file.description:
                    f.write(f"{parsed_file.description}\n")
                f.write(f"- Functions: {len(parsed_file.functions)}\n")
                f.write(f"- Structures: {len(parsed_file.structures)}\n")
                f.write(f"- Enums: {len(parsed_file.enums)}\n\n")
    
    def _generate_function_reference(self, parsed_files: List[HeaderFileInfo], output_dir: str) -> None:
        """Generate function reference documentation"""
        output_file = os.path.join(output_dir, "functions.md")
        
        with open(output_file, 'w') as f:
            f.write("# Function Reference\n\n")
            f.write("Complete reference of all public functions in the AutoWatering API.\n\n")
            
            # Group functions by module
            functions_by_module = {}
            for parsed_file in parsed_files:
                module_name = os.path.basename(parsed_file.file_path).replace('.h', '')
                functions_by_module[module_name] = parsed_file.functions
            
            for module_name, functions in functions_by_module.items():
                if not functions:
                    continue
                    
                f.write(f"## {module_name}\n\n")
                
                for func in functions:
                    if func.is_static:
                        continue  # Skip static functions in public API
                    
                    f.write(f"### `{func.name}`\n\n")
                    f.write(f"**Signature**: `{func.return_type} {func.name}(`")
                    
                    if func.parameters:
                        param_strs = []
                        for param in func.parameters:
                            param_strs.append(f"{param['type']} {param['name']}")
                        f.write(", ".join(param_strs))
                    
                    f.write(")`\n\n")
                    
                    if func.description:
                        f.write(f"**Description**: {func.description}\n\n")
                    
                    if func.parameters:
                        f.write("**Parameters**:\n")
                        for param in func.parameters:
                            f.write(f"- `{param['name']}` ({param['type']})")
                            if param['description']:
                                f.write(f": {param['description']}")
                            f.write("\n")
                        f.write("\n")
                    
                    f.write(f"**Returns**: {func.return_type}\n\n")
                    f.write(f"**Defined in**: {os.path.basename(func.file_path)}:{func.line_number}\n\n")
                    f.write("---\n\n")
    
    def _generate_data_structures_reference(self, parsed_files: List[HeaderFileInfo], output_dir: str) -> None:
        """Generate data structures reference documentation"""
        output_file = os.path.join(output_dir, "data-structures.md")
        
        with open(output_file, 'w') as f:
            f.write("# Data Structures Reference\n\n")
            f.write("Complete reference of all data structures in the AutoWatering API.\n\n")
            
            # Structures
            f.write("## Structures\n\n")
            for parsed_file in parsed_files:
                for struct in parsed_file.structures:
                    f.write(f"### `{struct.name}`\n\n")
                    
                    if struct.description:
                        f.write(f"**Description**: {struct.description}\n\n")
                    
                    if struct.is_packed:
                        f.write("**Attributes**: `__packed`\n\n")
                    
                    f.write("**Fields**:\n")
                    for field in struct.fields:
                        f.write(f"- `{field['name']}` ({field['type']})")
                        if field['array_size']:
                            f.write(f"[{field['array_size']}]")
                        if field['description']:
                            f.write(f": {field['description']}")
                        f.write("\n")
                    f.write("\n")
                    
                    f.write(f"**Defined in**: {os.path.basename(struct.file_path)}:{struct.line_number}\n\n")
                    f.write("---\n\n")
            
            # Enumerations
            f.write("## Enumerations\n\n")
            for parsed_file in parsed_files:
                for enum in parsed_file.enums:
                    f.write(f"### `{enum.name}`\n\n")
                    
                    if enum.description:
                        f.write(f"**Description**: {enum.description}\n\n")
                    
                    f.write("**Values**:\n")
                    for value in enum.values:
                        f.write(f"- `{value['name']}` = {value['value']}")
                        if value['description']:
                            f.write(f": {value['description']}")
                        f.write("\n")
                    f.write("\n")
                    
                    f.write(f"**Defined in**: {os.path.basename(enum.file_path)}:{enum.line_number}\n\n")
                    f.write("---\n\n")
    
    def _generate_cross_references(self, parsed_files: List[HeaderFileInfo], output_dir: str) -> None:
        """Generate cross-reference mappings"""
        output_file = os.path.join(output_dir, "cross-references.json")
        
        cross_refs = {
            'functions': {},
            'structures': {},
            'enums': {},
            'dependencies': {}
        }
        
        # Build cross-reference data
        for parsed_file in parsed_files:
            filename = os.path.basename(parsed_file.file_path)
            
            # Function cross-references
            for func in parsed_file.functions:
                cross_refs['functions'][func.name] = {
                    'file': filename,
                    'line': func.line_number,
                    'return_type': func.return_type,
                    'parameters': [p['type'] for p in func.parameters]
                }
            
            # Structure cross-references
            for struct in parsed_file.structures:
                cross_refs['structures'][struct.name] = {
                    'file': filename,
                    'line': struct.line_number,
                    'fields': [f['type'] for f in struct.fields]
                }
            
            # Enum cross-references
            for enum in parsed_file.enums:
                cross_refs['enums'][enum.name] = {
                    'file': filename,
                    'line': enum.line_number,
                    'values': [v['name'] for v in enum.values]
                }
            
            # Dependencies
            cross_refs['dependencies'][filename] = parsed_file.includes
        
        with open(output_file, 'w') as f:
            json.dump(cross_refs, f, indent=2)
    
    def _generate_file_documentation(self, parsed_file: HeaderFileInfo, output_dir: str) -> None:
        """Generate documentation for individual header file"""
        filename = os.path.basename(parsed_file.file_path)
        output_file = os.path.join(output_dir, filename.replace('.h', '.md'))
        
        with open(output_file, 'w') as f:
            f.write(f"# {filename}\n\n")
            
            if parsed_file.description:
                f.write(f"{parsed_file.description}\n\n")
            
            # Includes
            if parsed_file.includes:
                f.write("## Dependencies\n\n")
                for include in parsed_file.includes:
                    f.write(f"- `{include}`\n")
                f.write("\n")
            
            # Functions
            if parsed_file.functions:
                f.write("## Functions\n\n")
                for func in parsed_file.functions:
                    f.write(f"- [`{func.name}`](functions.md#{func.name.lower()})")
                    if func.description:
                        f.write(f" - {func.description}")
                    f.write("\n")
                f.write("\n")
            
            # Structures
            if parsed_file.structures:
                f.write("## Data Structures\n\n")
                for struct in parsed_file.structures:
                    f.write(f"- [`{struct.name}`](data-structures.md#{struct.name.lower()})")
                    if struct.description:
                        f.write(f" - {struct.description}")
                    f.write("\n")
                f.write("\n")
            
            # Enums
            if parsed_file.enums:
                f.write("## Enumerations\n\n")
                for enum in parsed_file.enums:
                    f.write(f"- [`{enum.name}`](data-structures.md#{enum.name.lower()})")
                    if enum.description:
                        f.write(f" - {enum.description}")
                    f.write("\n")
                f.write("\n")

def main():
    """Main entry point"""
    parser = argparse.ArgumentParser(description="Generate AutoWatering API documentation")
    parser.add_argument("--source-dir", default="src/", help="Source directory to scan")
    parser.add_argument("--output-dir", default="docs/api/", help="Output directory for documentation")
    parser.add_argument("--format", default="markdown", choices=["markdown", "json", "html"], help="Output format")
    parser.add_argument("--verbose", action="store_true", help="Enable verbose logging")
    
    args = parser.parse_args()
    
    if args.verbose:
        logging.getLogger().setLevel(logging.DEBUG)
    
    # Generate documentation
    generator = APIDocumentationGenerator(args.format)
    generator.generate_documentation(args.source_dir, args.output_dir)

if __name__ == "__main__":
    main()