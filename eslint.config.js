import js from "@eslint/js";
import tseslint from "typescript-eslint";
import react from "eslint-plugin-react";

export default tseslint.config(
  js.configs.recommended,
  ...tseslint.configs.recommended,
  {
    files: ["**/*.{ts,tsx}"],
    plugins: { react },
    rules: {
      // Text from the model, an app or the system is only ever a React child, never markup.
      "react/no-danger": "error",
      "react/no-danger-with-children": "error",
      "no-restricted-syntax": [
        "error",
        { selector: "AssignmentExpression[left.property.name=/^(innerHTML|outerHTML)$/]", message: "Never build markup from text." },
      ],
    },
  },
  { ignores: ["**/dist/**", "**/node_modules/**", "**/bindings/**"] },
);
