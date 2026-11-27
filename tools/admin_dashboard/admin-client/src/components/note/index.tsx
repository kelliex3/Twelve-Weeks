import type { ReactNode } from "react";

interface NoteProps {
  children?: ReactNode;
  bad?: boolean;
}

export const Note = ({ children, bad }: NoteProps) => (
  <div className={`note ${bad ? "bad" : ""}`}>{children}</div>
);
