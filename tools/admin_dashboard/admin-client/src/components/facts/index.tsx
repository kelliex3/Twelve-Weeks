import type { ReactNode } from "react";

interface FactProps {
  label: string;
  value?: string | number | null;
  subtitle?: ReactNode;
  className?: string;
}

export const Fact = ({ label, value, subtitle, className }: FactProps) => (
  <div className={`fact ${className || ""}`}>
    <label>{label}</label>
    <b>{value == null ? "" : String(value)}</b>
    {subtitle ? <span>{subtitle}</span> : null}
  </div>
);

interface FactsProps {
  children?: ReactNode;
}

export const Facts = ({ children }: FactsProps) => (
  <div className="facts">{children}</div>
);
